#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp-tinybft.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_mac.h"

#define TAG "counter"

#if CONFIG_TBFT_TRANSPORT_UDP
#include "esp_event.h"
#include "freertos/event_groups.h"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int s_retry = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry++;
        ESP_LOGW(TAG, "WiFi disconnected (attempt %d), reconnecting...", s_retry);
        /* bounded backoff: 100 ms .. 2 s, avoids hammering a missing AP */
        int back_ms = s_retry * 100;
        vTaskDelay(pdMS_TO_TICKS(back_ms > 2000 ? 2000 : back_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
#endif

#if CONFIG_EXAMPLE_ROLE_REPLICA
#define EXAMPLE_ROLE_STR "replica"
#else
#define EXAMPLE_ROLE_STR "client"
#endif

#if CONFIG_EXAMPLE_ROLE_CLIENT
#define EXAMPLE_REQ_INTERVAL_MS CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS
#else
#define EXAMPLE_REQ_INTERVAL_MS 0
#endif

static int32_t counter_state[4096 / sizeof(int32_t)];

typedef struct {
    uint8_t op;
} __attribute__((packed)) counter_req_t;

typedef struct {
    int32_t value;
    int8_t status;
} __attribute__((packed)) counter_rep_t;

int exec_cb(Byz_req *in, Byz_rep *out, Byz_buffer *ndet, int cid, bool ro) {
    if (in->size < 68) return -1;
    counter_req_t *req = (counter_req_t *)((const uint8_t *)in->contents + 68);
    counter_rep_t *rep = (counter_rep_t *)out->contents;
    out->size = sizeof(counter_rep_t);

    if (req->op == 0) {
        Byz_modify(&counter_state[0], sizeof(int32_t));
        counter_state[0]++;
        rep->value = counter_state[0];
        rep->status = 0;
        ESP_LOGI(TAG, "Incremented counter to %ld", counter_state[0]);
    } else if (req->op == 1) {
        rep->value = counter_state[0];
        rep->status = 0;
        ESP_LOGI(TAG, "Read counter = %ld", counter_state[0]);
    } else {
        return -1;
    }
    return 0;
}

/* Printed at WARN so it survives CONFIG_LOG_DEFAULT_LEVEL_WARN: proves which
 * experiment variant each board runs. */
static void print_banner(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#if CONFIG_TBFT_TRANSPORT_UDP
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
    ESP_LOGW(TAG, "BANNER role=%s transport=udp ps=%s listen_interval=%d ch=%d "
             "req_interval_ms=%d ver=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
             EXAMPLE_ROLE_STR,
             ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max",
             CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL, ch,
             EXAMPLE_REQ_INTERVAL_MS, __DATE__ " " __TIME__,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#else
    ESP_LOGW(TAG, "BANNER role=%s transport=espnow if=%s ps=n/a "
             "req_interval_ms=%d ver=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
             EXAMPLE_ROLE_STR,
#if CONFIG_TBFT_ESPNOW_IF_STA
             "sta",
#else
             "ap",
#endif
             EXAMPLE_REQ_INTERVAL_MS, __DATE__ " " __TIME__,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_TBFT_TRANSPORT_UDP
    ESP_LOGI(TAG, "Connecting to WiFi (SSID: %s)...", CONFIG_EXAMPLE_WIFI_SSID);
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));
    wifi_config_t wifi_cfg = {
        .sta = { .ssid = CONFIG_EXAMPLE_WIFI_SSID,
                 .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
                 .listen_interval = CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_EXAMPLE_WIFI_PS_NONE
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi modem-sleep disabled (WIFI_PS_NONE)");
#elif CONFIG_EXAMPLE_WIFI_PS_MAX_MODEM
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));
    ESP_LOGI(TAG, "WiFi modem-sleep: maximum (WIFI_PS_MAX_MODEM)");
#else
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    ESP_LOGI(TAG, "WiFi modem-sleep: minimum (WIFI_PS_MIN_MODEM)");
#endif
    esp_wifi_connect();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi connected");
#else
#if CONFIG_TBFT_ESPNOW_IF_STA
    /* Station interface, never connected: no beacons, no AP forcing RF on.
     * Must match TBFT_ESPNOW_IF on every board. */
    ESP_LOGI(TAG, "Starting WiFi STA for ESP-NOW (channel %d)...", CONFIG_EXAMPLE_ESPNOW_CHANNEL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_EXAMPLE_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
#else
    ESP_LOGI(TAG, "Starting WiFi AP for ESP-NOW...");
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "tinybft",
            .ssid_len = 7,
            .channel = CONFIG_EXAMPLE_ESPNOW_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 0,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
#endif
    ESP_ERROR_CHECK(esp_now_init());
#endif
}

/* A node that cannot find itself in the config would otherwise sit idle with
 * the radio on, silently invalidating any energy measurement. Retry a few
 * times (DHCP/IP may lag), then reboot loudly. */
static int detect_local_id_or_reboot(const char *config_file) {
    for (int attempt = 1; attempt <= 10; attempt++) {
        int id = Byz_detect_local_id(config_file);
        if (id >= 0) return id;
        ESP_LOGE(TAG, "local id not found in %s (attempt %d/10) - check this "
                 "board's MAC/IP (DHCP reservation) against the config",
                 config_file, attempt);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGE(TAG, "giving up: restarting");
    esp_restart();
    return -1;
}

#if CONFIG_EXAMPLE_ROLE_REPLICA
static void replica_task(void *arg) {
    const char *config_file = "/spiffs/config_udp.txt";
#if CONFIG_TBFT_TRANSPORT_ESPNOW
    config_file = "/spiffs/config_espnow.txt";
#endif

    int local_id = detect_local_id_or_reboot(config_file);
    ESP_LOGI(TAG, "Using local_id=%d", local_id);

    char priv_config[32];
    snprintf(priv_config, sizeof(priv_config), "/spiffs/priv%d.der", local_id);

    counter_state[0] = 0;

    int ret = Byz_init_replica(config_file, priv_config,
                               local_id,
                               counter_state, sizeof(counter_state),
                               exec_cb, NULL, 0, NULL, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to init replica");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Replica running...");
    Byz_replica_run();
}
#else
static void client_task(void *arg) {
    const char *config_file = "/spiffs/config_udp.txt";
#if CONFIG_TBFT_TRANSPORT_ESPNOW
    config_file = "/spiffs/config_espnow.txt";
#endif

    vTaskDelay(pdMS_TO_TICKS(5000));

    int local_id = detect_local_id_or_reboot(config_file);
    ESP_LOGI(TAG, "Client detected local_id=%d", local_id);

    char priv_config[32];
    snprintf(priv_config, sizeof(priv_config), "/spiffs/priv%d.der", local_id);

    int ret = Byz_init_client(config_file, priv_config, local_id, 0);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to init client");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Client running...");

#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(NULL);
#endif

    const TickType_t period = pdMS_TO_TICKS(CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS);
    TickType_t next = xTaskGetTickCount();
    uint32_t n_ok = 0, n_fail = 0, n_missed = 0;
    int64_t lat_min = INT64_MAX, lat_max = 0, lat_sum = 0;

    while (1) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        Byz_req req;
        Byz_rep rep;
        bool ok = false;
        int64_t t0 = esp_timer_get_time();
        if (Byz_alloc_request(&req, sizeof(counter_req_t)) == 0) {
            counter_req_t *cmd = (counter_req_t *)req.contents;
            cmd->op = 0;
            req.size = sizeof(counter_req_t);
            if (Byz_invoke(&req, &rep, false) == 0) {
                counter_rep_t *res = (counter_rep_t *)rep.contents;
                ESP_LOGI(TAG, "Client reply: value=%ld, status=%d",
                         res->value, res->status);
                Byz_free_reply(&rep);
                ok = true;
            }
            Byz_free_request(&req);
        } else {
            ESP_LOGE(TAG, "Failed to allocate request");
        }
        int64_t lat_ms = (esp_timer_get_time() - t0) / 1000;
        if (ok) {
            n_ok++;
            lat_sum += lat_ms;
            if (lat_ms < lat_min) lat_min = lat_ms;
            if (lat_ms > lat_max) lat_max = lat_ms;
        } else {
            n_fail++;
        }
        /* machine-parsable summary, WARN so it survives the measure overlay */
        ESP_LOGW(TAG, "CLIENT_STATS ok=%lu fail=%lu missed=%lu last_ms=%lld "
                 "min_ms=%lld avg_ms=%lld max_ms=%lld",
                 (unsigned long)n_ok, (unsigned long)n_fail,
                 (unsigned long)n_missed, (long long)lat_ms,
                 (long long)(n_ok ? lat_min : 0),
                 (long long)(n_ok ? lat_sum / n_ok : 0), (long long)lat_max);
#if CONFIG_TBFT_PRINT_STATS
        Byz_print_stats();
#endif

        /* Fixed period: next slot is absolute, a slow request does not shift
         * the schedule; slots already past are skipped and counted. */
        next += period;
        while ((int32_t)(next - xTaskGetTickCount()) <= 0) {
            next += period;
            n_missed++;
        }
        while ((int32_t)(next - xTaskGetTickCount()) > 0) {
            TickType_t left = next - xTaskGetTickCount();
            TickType_t chunk = left > pdMS_TO_TICKS(1000) ? pdMS_TO_TICKS(1000) : left;
            vTaskDelay(chunk);
#if CONFIG_ESP_TASK_WDT_EN
            esp_task_wdt_reset();
#endif
        }
    }
}
#endif

void app_main(void) {
    ESP_LOGI(TAG, "=== Counter starting ===");
    ESP_LOGI(TAG, "ESP-IDF: %s, Target: ESP32-C3", IDF_VER);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");

    wifi_init();
    print_banner();

#if CONFIG_EXAMPLE_ROLE_REPLICA
    xTaskCreate(replica_task, "replica_task", 8192, NULL, 5, NULL);
#else
    xTaskCreate(client_task, "client_task", 8192, NULL, 5, NULL);
#endif
}
