/*
 * sim_client — the counter example's client as a Linux process.
 *
 *   sim_client <id> <config_file> <period_ms> <count>
 *              [--start-n N] [--boot-delay-ms MS] [--priv FILE]
 *
 * Mirrors client_task() in examples/counter/main/main.c: after a boot delay
 * (5000 ms by default, as in the example), Byz_init_client() and then one
 * increment request every period_ms on a fixed schedule (slots overrun by a
 * slow request are skipped and counted as "missed"), a CLIENT_STATS line
 * after each request, Byz_print_stats() when TBFT_PRINT_STATS is on.
 *
 * Each request carries a token ((SIM_INCARNATION << 32) | n) after the op
 * byte so the checker can match executions to requests.  Extra lines:
 *   SIM_REQ_START n= token= rid=
 *   SIM_REQ_END   n= token= ok= value= status= lat_ms=
 *   SIM_CLIENT_DONE ok= fail= missed=
 */
#define _GNU_SOURCE
#include "esp-tinybft.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sim_rt.h"
#include "sim_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static constexpr char TAG[] = "counter";

static const char *g_config;
static char        g_priv[512];
static int         g_id;
static int         g_period_ms;
static int         g_count;
static int         g_start_n = 1;
static int         g_boot_delay_ms = 5000;

static void client_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Client, local_id=%d", g_id);
    vTaskDelay(pdMS_TO_TICKS(g_boot_delay_ms));   /* let the replicas boot */

    if (Byz_init_client(g_config, g_priv, g_id, 0) != 0) {
        sim_fatal_reboot("Byz_init_client failed");
    }
    ESP_LOGI(TAG, "Client running...");
#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_add(nullptr);
#endif

    const TickType_t period = pdMS_TO_TICKS(g_period_ms);
    TickType_t next = xTaskGetTickCount();
    uint32_t n_ok = 0, n_fail = 0, n_missed = 0;
    int64_t lat_min = INT64_MAX, lat_max = 0, lat_sum = 0;
    uint64_t sent_in_this_boot = 0;

    for (int i = 0; i < g_count; i++) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif
        uint32_t n = (uint32_t)(g_start_n + i);
        uint64_t token = ((uint64_t)(uint32_t)sim_rt_incarnation() << 32) | n;
        Byz_req req;
        Byz_rep rep;
        bool ok = false;
        int value = 0, status = 0;
        int64_t t0 = esp_timer_get_time();
        if (Byz_alloc_request(&req, sizeof(sim_counter_req_t)) == 0) {
            sim_counter_req_t cmd = { .op = 0, .token = token };
            memcpy(req.contents, &cmd, sizeof(cmd));
            req.size = sizeof(cmd);
            sent_in_this_boot++;   /* Byz_send_request takes the next rid */
            sim_printf("SIM_REQ_START n=%u token=%016llx rid=%llu", n,
                       (unsigned long long)token, (unsigned long long)sent_in_this_boot);
            if (Byz_invoke(&req, &rep, false) == 0) {
                if (rep.size >= (int)sizeof(sim_counter_rep_t)) {
                    sim_counter_rep_t res;
                    memcpy(&res, rep.contents, sizeof(res));
                    value = res.value;
                    status = res.status;
                }
                ESP_LOGI(TAG, "Client reply: value=%d, status=%d", value, status);
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
        sim_printf("SIM_REQ_END n=%u token=%016llx ok=%d value=%d status=%d lat_ms=%lld",
                   n, (unsigned long long)token, ok ? 1 : 0, value, status,
                   (long long)lat_ms);
        ESP_LOGW(TAG, "CLIENT_STATS ok=%lu fail=%lu missed=%lu last_ms=%lld "
                 "min_ms=%lld avg_ms=%lld max_ms=%lld",
                 (unsigned long)n_ok, (unsigned long)n_fail,
                 (unsigned long)n_missed, (long long)lat_ms,
                 (long long)(n_ok ? lat_min : 0),
                 (long long)(n_ok ? lat_sum / n_ok : 0), (long long)lat_max);
#if CONFIG_TBFT_PRINT_STATS
        Byz_print_stats();
#endif
        if (i + 1 == g_count) break;

        /* Fixed period, exactly as the example's client_task(). */
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
    sim_printf("SIM_CLIENT_DONE ok=%u fail=%u missed=%u", n_ok, n_fail, n_missed);
    sim_log_flush();
    _exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s <id> <config_file> <period_ms> <count> "
                        "[--start-n N] [--boot-delay-ms MS] [--priv FILE]\n", argv[0]);
        return 2;
    }
    g_id = atoi(argv[1]);
    g_config = argv[2];
    g_period_ms = atoi(argv[3]);
    g_count = atoi(argv[4]);
    sim_default_priv_path(g_config, g_id, g_priv, sizeof(g_priv));
    for (int i = 5; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--start-n") == 0) {
            g_start_n = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--boot-delay-ms") == 0) {
            g_boot_delay_ms = atoi(argv[i + 1]);
        } else if (strcmp(argv[i], "--priv") == 0) {
            snprintf(g_priv, sizeof(g_priv), "%s", argv[i + 1]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (g_period_ms < 1) g_period_ms = 1;
    sim_rt_set_node(g_id, "client");

    char ip[64];
    int num_nodes = 0, num_replicas = 0;
    if (sim_cfg_lookup(g_config, g_id, ip, sizeof(ip), &num_nodes, &num_replicas) != 0 ||
        sim_net_set_local_ip(ip) != 0) {
        fprintf(stderr, "node %d not found in %s\n", g_id, g_config);
        return 2;
    }
    sim_block_term_signals();
    sim_printf("SIM_BOOT role=client id=%d incarnation=%d pid=%d ip=%s period_ms=%d "
               "count=%d start_n=%d boot_delay_ms=%d reply_timeout_ms=%d",
               g_id, sim_rt_incarnation(), (int)getpid(), ip, g_period_ms, g_count,
               g_start_n, g_boot_delay_ms, CONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS);

    xTaskCreate(client_task, "client_task", 8192, nullptr, 5, nullptr);

    for (;;) {
        int sig = sim_wait_term_signal(1000);
        if (sig) {
            sim_printf("SIM_EXIT signal=%d", sig);
            sim_log_flush();
            _exit(0);
        }
    }
}
