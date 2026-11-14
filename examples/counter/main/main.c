#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
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
#include "tbft_config.h"   /* TBFT_ESPNOW_RETRY_* printed in the banner */
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_mac.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static constexpr char TAG[] = "counter";

#if CONFIG_TBFT_TRANSPORT_UDP && CONFIG_EXAMPLE_STATIC_IP
/* Return the 0-based node index whose MAC matches this board in
 * config_espnow.txt (header: name, f, auth, num_nodes, mcast; then one
 * "<host> <mac> <key>" line per node). -1 if not found. */
static int node_index_by_mac(void) {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return -1;
    char want[18];
    snprintf(want, sizeof(want), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    FILE *f = fopen("/spiffs/config_espnow.txt", "r");
    if (!f) return -1;
    char line[160];
    int lineno = 0, nodes = 0, idx = -1;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (lineno == 4) nodes = atoi(line);
        if (lineno <= 5) continue;
        if (lineno - 6 >= nodes) break;
        char host[64], m[32];
        if (sscanf(line, "%63s %31s", host, m) == 2 && strcasecmp(m, want) == 0) {
            idx = lineno - 6;
            break;
        }
    }
    fclose(f);
    return idx;
}

/* Return the IP string at node index idx in config_udp.txt (same layout). */
static bool ip_for_index(int idx, char *out, size_t out_len) {
    FILE *f = fopen("/spiffs/config_udp.txt", "r");
    if (!f) return false;
    char line[160];
    int lineno = 0;
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (lineno - 6 == idx) {
            char host[64], ip[32];
            if (sscanf(line, "%63s %31s", host, ip) == 2) {
                snprintf(out, out_len, "%s", ip);
                ok = true;
            }
            break;
        }
    }
    fclose(f);
    return ok;
}

static esp_netif_ip_info_t s_static_ip;
static bool s_static_ip_ok;

/* Look the address up once (file I/O, main task). */
static void resolve_static_ip(void) {
    int idx = node_index_by_mac();
    char ip[32];
    if (idx < 0 || !ip_for_index(idx, ip, sizeof(ip))) {
        ESP_LOGE(TAG, "static IP: this board's MAC is not in config_espnow.txt - "
                 "falling back to DHCP");
        return;
    }
    if (esp_netif_str_to_ip4(ip, &s_static_ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_EXAMPLE_STATIC_IP_NETMASK, &s_static_ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_EXAMPLE_STATIC_IP_GATEWAY, &s_static_ip.gw) != ESP_OK) {
        ESP_LOGE(TAG, "static IP: bad address in config/Kconfig - falling back to DHCP");
        return;
    }
    s_static_ip_ok = true;
    ESP_LOGW(TAG, "static IP %s (node index %d)", ip, idx);
}

/* Called on every WIFI_EVENT_STA_CONNECTED, the same sequence as ESP-IDF's
 * examples/protocols/static_ip: the default handler has just started DHCP,
 * so stop it and set the address; esp_netif then posts IP_EVENT_STA_GOT_IP. */
static void apply_static_ip(esp_netif_t *netif) {
    if (!s_static_ip_ok || !netif) return;
    esp_err_t e = esp_netif_dhcpc_stop(netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "static IP: dhcpc_stop failed: 0x%x", e);
        return;
    }
    e = esp_netif_set_ip_info(netif, &s_static_ip);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "static IP: set_ip_info failed: 0x%x", e);
    }
}
#endif

#if CONFIG_TBFT_TRANSPORT_UDP
#include "esp_event.h"
#include "freertos/event_groups.h"

static EventGroupHandle_t s_wifi_event_group;
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int s_retry = 0;
#if CONFIG_EXAMPLE_STATIC_IP
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        apply_static_ip((esp_netif_t *)arg);
        return;
    }
#endif
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

#if CONFIG_PM_ENABLE
static constexpr int EXAMPLE_PM = 1;
#else
static constexpr int EXAMPLE_PM = 0;
#endif

/* Role (menuconfig "Role of this board"): read from the node list at boot,
 * or forced.  A forced image only contains the code of its role. */
#if CONFIG_EXAMPLE_ROLE_AUTO
static constexpr char EXAMPLE_ROLE_FROM[] = "list";
#else
static constexpr char EXAMPLE_ROLE_FROM[] = "forced";
#endif
#ifdef CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS
static constexpr int EXAMPLE_REQ_INTERVAL_MS = CONFIG_EXAMPLE_CLIENT_REQUEST_INTERVAL_MS;
#else
static constexpr int EXAMPLE_REQ_INTERVAL_MS = 0;
#endif

#if CONFIG_TBFT_TRANSPORT_ESPNOW
static constexpr char EXAMPLE_CONFIG_FILE[] = "/spiffs/config_espnow.txt";
#else
static constexpr char EXAMPLE_CONFIG_FILE[] = "/spiffs/config_udp.txt";
#endif

static int32_t counter_state[4096 / sizeof(int32_t)];

typedef struct [[gnu::packed]] {
    uint8_t op;
} counter_req_t;

typedef struct [[gnu::packed]] {
    int32_t value;
    int8_t status;
} counter_rep_t;

int exec_cb(Byz_req *in, Byz_rep *out, Byz_buffer *ndet, int cid, bool ro) {
    if (in->size < BYZ_REQ_HEADER_SIZE) return -1;
    counter_req_t *req = (counter_req_t *)((const uint8_t *)in->contents + BYZ_REQ_HEADER_SIZE);
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
        /* Unknown op: reply with an error status (deterministic on all
         * replicas) instead of returning -1, so the client is not left
         * waiting for a reply that will never come. */
        rep->value = 0;
        rep->status = -1;
    }
    return 0;
}

/* Banner fields.  Each transport prints only its own, hence [[maybe_unused]]. */
#if CONFIG_TBFT_DISABLE_MULTICAST
[[maybe_unused]] static constexpr int EXAMPLE_MCAST = 0;
#else
[[maybe_unused]] static constexpr int EXAMPLE_MCAST = 1;
#endif
#if CONFIG_EXAMPLE_STATIC_IP
[[maybe_unused]] static constexpr int EXAMPLE_STATIC_IP_ON = 1;
#else
[[maybe_unused]] static constexpr int EXAMPLE_STATIC_IP_ON = 0;
#endif
#if CONFIG_TBFT_ESPNOW_BROADCAST
[[maybe_unused]] static constexpr int EXAMPLE_BCAST = 1;
#else
[[maybe_unused]] static constexpr int EXAMPLE_BCAST = 0;
#endif
#if CONFIG_TBFT_ESPNOW_POWER_SAVE
[[maybe_unused]] static constexpr int EXAMPLE_EN_WINDOW   = CONFIG_TBFT_ESPNOW_WAKE_WINDOW_MS;
[[maybe_unused]] static constexpr int EXAMPLE_EN_INTERVAL = CONFIG_TBFT_ESPNOW_WAKE_INTERVAL_MS;
#else
[[maybe_unused]] static constexpr int EXAMPLE_EN_WINDOW   = 0;  /* 0/0 = receiver always on */
[[maybe_unused]] static constexpr int EXAMPLE_EN_INTERVAL = 0;
#endif
#if CONFIG_EXAMPLE_WIFI_PS_MAX_MODEM
[[maybe_unused]] static constexpr int EXAMPLE_LISTEN_INTERVAL = CONFIG_EXAMPLE_WIFI_LISTEN_INTERVAL;
#else
[[maybe_unused]] static constexpr int EXAMPLE_LISTEN_INTERVAL = 0;  /* only used with WIFI_PS_MAX_MODEM */
#endif

/* Printed at WARN so it survives CONFIG_LOG_DEFAULT_LEVEL_WARN: proves which
 * configuration each board runs (every menuconfig setting of the experiment,
 * plus the role found in the node list and the node id). */
static void print_banner(bool replica, int local_id) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint8_t ch = 0; wifi_second_chan_t sc;
    esp_wifi_get_channel(&ch, &sc);
#if CONFIG_TBFT_TRANSPORT_UDP
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    ESP_LOGW(TAG, "BANNER role=%s role_from=%s id=%d transport=udp ps=%s listen_interval=%d "
             "min_active_ms=%d mcast=%d static_ip=%d ch=%d pm=%d hz=%d "
             "idle_wait_ms=%d req_interval_ms=%d build=%s "
             "mac=%02x:%02x:%02x:%02x:%02x:%02x",
             replica ? "replica" : "client", EXAMPLE_ROLE_FROM, local_id,
             ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min" : "max",
             EXAMPLE_LISTEN_INTERVAL,
             CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME,
             EXAMPLE_MCAST, EXAMPLE_STATIC_IP_ON, ch,
             EXAMPLE_PM, CONFIG_FREERTOS_HZ, CONFIG_TBFT_IDLE_WAIT_MS,
             replica ? 0 : EXAMPLE_REQ_INTERVAL_MS, __DATE__ " " __TIME__,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#else
    ESP_LOGW(TAG, "BANNER role=%s role_from=%s id=%d transport=espnow if=%s rate=%d retries=%d "
             "retry_delay_ms=%d retry_span_ms=%d window_ms=%d interval_ms=%d bcast=%d ch=%d "
             "pm=%d hz=%d idle_wait_ms=%d req_interval_ms=%d build=%s "
             "mac=%02x:%02x:%02x:%02x:%02x:%02x",
             replica ? "replica" : "client", EXAMPLE_ROLE_FROM, local_id,
#if CONFIG_TBFT_ESPNOW_IF_STA
             "sta",
#else
             "ap",
#endif
             CONFIG_TBFT_ESPNOW_PHY_RATE, CONFIG_TBFT_ESPNOW_TX_RETRIES,
             TBFT_ESPNOW_RETRY_DELAY_MS, TBFT_ESPNOW_RETRY_SPAN_MS,
             EXAMPLE_EN_WINDOW, EXAMPLE_EN_INTERVAL, EXAMPLE_BCAST, ch,
             EXAMPLE_PM, CONFIG_FREERTOS_HZ, CONFIG_TBFT_IDLE_WAIT_MS,
             replica ? 0 : EXAMPLE_REQ_INTERVAL_MS, __DATE__ " " __TIME__,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#endif
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_TBFT_TRANSPORT_UDP
    ESP_LOGI(TAG, "Connecting to WiFi (SSID: %s)...", CONFIG_EXAMPLE_WIFI_SSID);
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
#if CONFIG_EXAMPLE_STATIC_IP
    resolve_static_ip();
#endif
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, sta_netif, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, &h2));
    wifi_config_t wifi_cfg = {
        .sta = { .ssid = CONFIG_EXAMPLE_WIFI_SSID,
                 .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
                 .listen_interval = EXAMPLE_LISTEN_INTERVAL },
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
#if CONFIG_TBFT_ESPNOW_POWER_SAVE
    /* Receive power saving (TinyBFT Configuration > ESP-NOW radio): the
     * transport sets the wake window and interval when it starts, right
     * after esp_now_init() as in ESP-IDF's examples/wifi/espnow. */
#else
    /* Receiver always on.  The default wake window (maximum) already keeps
     * the RF on; WIFI_PS_NONE says so explicitly, as the always-on bulb of
     * Espressif's esp-now coin_cell_demo does. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#endif
#endif
}

/* A node that cannot find itself in the config would otherwise sit idle with
 * the radio on, silently invalidating any energy measurement. Retry a few
 * times (DHCP/IP may lag), then reboot loudly. */
[[noreturn]] static void fatal_reboot(const char *what) {
    for (int i = 0; i < 3; i++) {
        ESP_LOGE(TAG, "%s - check this board's entry in %s and the role "
                 "(menuconfig: Counter Example Configuration > Role of this "
                 "board)", what, EXAMPLE_CONFIG_FILE);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    esp_restart();
}

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
}

#if !CONFIG_EXAMPLE_ROLE_CLIENT
static void replica_task(void *arg) {
    const char *config_file = EXAMPLE_CONFIG_FILE;
    int local_id = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Replica, local_id=%d", local_id);

    char priv_config[32];
    snprintf(priv_config, sizeof(priv_config), "/spiffs/priv%d.der", local_id);

    counter_state[0] = 0;

    int ret = Byz_init_replica(config_file, priv_config,
                               local_id,
                               counter_state, sizeof(counter_state),
                               exec_cb, nullptr, 0, nullptr, 0);
    if (ret != 0) {
        fatal_reboot("Byz_init_replica failed");
    }

    ESP_LOGI(TAG, "Replica running...");
    Byz_replica_run();
}
#endif

#if !CONFIG_EXAMPLE_ROLE_REPLICA
static void client_task(void *arg) {
    const char *config_file = EXAMPLE_CONFIG_FILE;
    int local_id = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Client, local_id=%d", local_id);

    vTaskDelay(pdMS_TO_TICKS(5000));   /* let the replicas finish booting */

    char priv_config[32];
    snprintf(priv_config, sizeof(priv_config), "/spiffs/priv%d.der", local_id);

    int ret = Byz_init_client(config_file, priv_config, local_id, 0);
    if (ret != 0) {
        fatal_reboot("Byz_init_client failed");
    }

    ESP_LOGI(TAG, "Client running...");

#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(nullptr);
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
      .partition_label = nullptr,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_LOGI(TAG, "SPIFFS mounted at /spiffs");

    wifi_init();
#if CONFIG_PM_ENABLE
    /* DFS + automatic light sleep, as in the ESP-IDF low-power guide:
     * max = chip maximum, min = XTAL frequency, light sleep when tickless
     * idle is on.  The radio must be allowed to sleep (Wi-Fi PS mode or an
     * ESP-NOW wake window) for this to have any effect. */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true,
#endif
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
#endif

    /* Who am I?  The node list on SPIFFS says: entries named node* are
     * replicas, any other entry is a client (Byz_node_is_replica). */
    int local_id = detect_local_id_or_reboot(EXAMPLE_CONFIG_FILE);
    int listed_replica = Byz_node_is_replica(EXAMPLE_CONFIG_FILE, local_id);
    if (listed_replica < 0) {
        fatal_reboot("cannot read the node list");
    }
#if CONFIG_EXAMPLE_ROLE_REPLICA
    if (!listed_replica) {
        fatal_reboot("this board is listed as a client, but the image is forced to Replica");
    }
#elif CONFIG_EXAMPLE_ROLE_CLIENT
    if (listed_replica) {
        fatal_reboot("this board is listed as a replica, but the image is forced to Client");
    }
#endif
    print_banner(listed_replica == 1, local_id);

#if !CONFIG_EXAMPLE_ROLE_CLIENT
    if (listed_replica == 1) {
        xTaskCreate(replica_task, "replica_task", 8192, (void *)(intptr_t)local_id, 5, nullptr);
        return;
    }
#endif
#if !CONFIG_EXAMPLE_ROLE_REPLICA
    xTaskCreate(client_task, "client_task", 8192, (void *)(intptr_t)local_id, 5, nullptr);
#endif
}
