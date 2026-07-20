#include <stdio.h>
#include <string.h>
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

#define TAG "counter"

#if CONFIG_TBFT_TRANSPORT_UDP
#include "esp_event.h"
#include "freertos/event_groups.h"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int s_retry = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        s_retry++;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
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
                 .password = CONFIG_EXAMPLE_WIFI_PASSWORD },
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
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                            pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGE(TAG, "WiFi connection failed — halting until reconnected");
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                            pdFALSE, pdFALSE, portMAX_DELAY);
        ESP_LOGI(TAG, "WiFi reconnected");
    }
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
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 0,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_now_init());
#endif
}

#if CONFIG_EXAMPLE_ROLE_REPLICA
static void replica_task(void *arg) {
    const char *config_file = "/spiffs/config_udp.txt";
#if CONFIG_TBFT_TRANSPORT_ESPNOW
    config_file = "/spiffs/config_espnow.txt";
#endif

    int local_id = Byz_detect_local_id(config_file);
    if (local_id < 0) {
        ESP_LOGE(TAG, "Failed to auto-detect local ID — "
                 "check that this board's MAC/IP is in the config file");
        vTaskDelete(NULL);
        return;
    }
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

    int local_id = Byz_detect_local_id(config_file);
    if (local_id < 0) {
        ESP_LOGE(TAG, "Failed to auto-detect local ID — "
                 "check that this board's MAC is in the config file");
        vTaskDelete(NULL);
        return;
    }
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

    while(1) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        Byz_req req;
        Byz_rep rep;
        if (Byz_alloc_request(&req, sizeof(counter_req_t)) != 0) {
            ESP_LOGE(TAG, "Failed to allocate request");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        counter_req_t *cmd = (counter_req_t *)req.contents;
        cmd->op = 0;
        req.size = sizeof(counter_req_t);

        ESP_LOGI(TAG, "Client sending increment...");
        if (Byz_invoke(&req, &rep, false) == 0) {
            counter_rep_t *res = (counter_rep_t *)rep.contents;
            ESP_LOGI(TAG, "Client reply: value=%ld, status=%d",
                     res->value, res->status);
            Byz_free_reply(&rep);
        } else {
            ESP_LOGW(TAG, "Request failed or timed out.");
        }

        Byz_free_request(&req);
        int remaining_ms = CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS;
        while (remaining_ms > 0) {
            int chunk_ms = remaining_ms > 1000 ? 1000 : remaining_ms;
            vTaskDelay(pdMS_TO_TICKS(chunk_ms));
#if CONFIG_ESP_TASK_WDT_EN
            esp_task_wdt_reset();
#endif
            remaining_ms -= chunk_ms;
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

#if CONFIG_EXAMPLE_ROLE_REPLICA
    xTaskCreate(replica_task, "replica_task", 8192, NULL, 5, NULL);
#else
    xTaskCreate(client_task, "client_task", 8192, NULL, 5, NULL);
#endif
}
