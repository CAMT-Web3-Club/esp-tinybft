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
        if (s_retry < 5) {
            esp_wifi_connect();
            s_retry++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
#endif

#define TAG "simple_wallet"

#define NUM_ACCOUNTS 4
static int32_t app_state[4096 / sizeof(int32_t)];

typedef struct {
    uint8_t op; // 0 = read, 1 = transfer
    uint8_t from;
    uint8_t to;
    int32_t amount;
} __attribute__((packed)) wallet_req_t;

typedef struct {
    int32_t balance_from;
    int32_t balance_to;
    int8_t status; // 0 = ok, -1 = error
} __attribute__((packed)) wallet_rep_t;

int exec_cb(Byz_req *in, Byz_rep *out, Byz_buffer *ndet, int cid, bool ro) {
    if (in->size != sizeof(wallet_req_t)) return -1;
    wallet_req_t *req = (wallet_req_t *)in->contents;
    wallet_rep_t *rep = (wallet_rep_t *)out->contents;
    out->size = sizeof(wallet_rep_t);

    if (req->op == 0) {
        if (req->from >= NUM_ACCOUNTS) return -1;
        rep->balance_from = app_state[req->from];
        rep->balance_to = 0;
        rep->status = 0;
    } else if (req->op == 1) {
        if (req->from >= NUM_ACCOUNTS || req->to >= NUM_ACCOUNTS) return -1;

        Byz_modify(&app_state[req->from], sizeof(int32_t));
        Byz_modify(&app_state[req->to], sizeof(int32_t));

        if (app_state[req->from] >= req->amount && req->amount > 0) {
            app_state[req->from] -= req->amount;
            app_state[req->to] += req->amount;
            rep->status = 0;
        } else {
            rep->status = -1;
        }
        rep->balance_from = app_state[req->from];
        rep->balance_to = app_state[req->to];
        ESP_LOGI(TAG, "Transfer %ld from %d to %d (status %d)", req->amount, req->from, req->to, rep->status);
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &h2));
    wifi_config_t wifi_cfg = {
        .sta = { .ssid = CONFIG_EXAMPLE_WIFI_SSID, .password = CONFIG_EXAMPLE_WIFI_PASSWORD },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
    } else {
        ESP_LOGE(TAG, "WiFi connection failed");
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

    /* Auto-detect local_id from MAC (ESP-NOW) or IP (UDP).
     * Config file must contain the matching address for this board. */
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

    // Initial balances
    app_state[0] = 1000;
    app_state[1] = 1000;
    app_state[2] = 1000;
    app_state[3] = 1000;

    int ret = Byz_init_replica(config_file, priv_config,
                               local_id,
                               app_state, sizeof(app_state),
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

    vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for replicas to start

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
        if (Byz_alloc_request(&req, sizeof(wallet_req_t)) != 0) {
            ESP_LOGE(TAG, "Failed to allocate request");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        wallet_req_t *cmd = (wallet_req_t *)req.contents;
        cmd->op = 1; // transfer
        cmd->from = 0;
        cmd->to = 1;
        cmd->amount = 10;
        req.size = sizeof(wallet_req_t);

        ESP_LOGI(TAG, "Client invoking transfer...");
        if (Byz_invoke(&req, &rep, false) == 0) {
            wallet_rep_t *res = (wallet_rep_t *)rep.contents;
            ESP_LOGI(TAG, "Client received reply: status=%d, balance0=%ld, balance1=%ld",
                     res->status, res->balance_from, res->balance_to);
            Byz_free_reply(&rep);
        } else {
            ESP_LOGW(TAG, "Request failed or timed out.");
        }

        Byz_free_request(&req);
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
#endif /* CONFIG_EXAMPLE_ROLE_REPLICA / else */

void app_main(void) {
    ESP_LOGI(TAG, "=== Simple Wallet starting ===");
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
