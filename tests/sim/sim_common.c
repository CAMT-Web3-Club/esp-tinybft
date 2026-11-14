#define _GNU_SOURCE
#include "sim_common.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sim_cfg_lookup(const char *path, int id, char *ip, size_t ip_len,
                   int *num_nodes_out, int *num_replicas_out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char name[64], mcast[64];
    int fault, auth, num_nodes;
    int rc = -1;
    if (fscanf(f, "%63s %d %d %d %63s", name, &fault, &auth, &num_nodes, mcast) != 5) {
        goto out;
    }
    int replicas = 0;
    for (int i = 0; i < num_nodes; i++) {
        char host[64], addr[64], key[256];
        int port;
        if (fscanf(f, "%63s %63s %d %255s", host, addr, &port, key) != 4) goto out;
        if (strncmp(host, "node", 4) == 0) replicas++;
        if (i == id && ip) snprintf(ip, ip_len, "%s", addr);
    }
    if (id < 0 || id >= num_nodes) goto out;
    if (num_nodes_out) *num_nodes_out = num_nodes;
    if (num_replicas_out) *num_replicas_out = replicas;
    rc = 0;
out:
    fclose(f);
    return rc;
}

void sim_default_priv_path(const char *config, int id, char *out, size_t len)
{
    const char *slash = strrchr(config, '/');
    if (slash) {
        snprintf(out, len, "%.*s/priv%d.key", (int)(slash - config), config, id);
    } else {
        snprintf(out, len, "priv%d.key", id);
    }
}

void sim_hex(const uint8_t *d, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[2 * n] = '\0';
}

void sim_block_term_signals(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

int sim_wait_term_signal(int ms)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    int sig = sigtimedwait(&set, nullptr, &ts);
    return sig > 0 ? sig : 0;
}

/* Same behaviour as fatal_reboot() in the counter example, with shorter
 * pauses: report, then reboot (the orchestrator restarts the process). */
void sim_fatal_reboot(const char *what)
{
    for (int i = 0; i < 3; i++) {
        ESP_LOGE("sim", "%s - check this node's entry in the config file", what);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_restart();
}
