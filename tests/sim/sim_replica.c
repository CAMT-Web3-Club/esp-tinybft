/*
 * sim_replica — one TinyBFT replica of the counter example as a Linux process.
 *
 *   sim_replica <id> <config_file> [priv_key_file]
 *
 * Mirrors examples/counter/main/main.c (replica role): a 4096-byte int32
 * state array, exec_cb increments counter_state[0] and replies
 * {int32 value, int8 status}; Byz_init_replica() + Byz_replica_run() run in
 * a FreeRTOS task ("replica_task").  No Wi-Fi/SPIFFS/NVS: the node's
 * address comes from its line in the config file.
 *
 * Extra output for the orchestrator (always printed, level letter 'S'):
 *   SIM_BOOT     once at start
 *   SIM_REPLICA  about every second, and once more with final=1 on SIGTERM
 *   SIM_EXEC     for every exec_cb call (seqno, view, value, request token)
 * plus a TBFT_STATS line on SIGTERM.
 */
#define _GNU_SOURCE
#include "esp-tinybft.h"
#include "tbft_replica.h"
#include "tbft_message.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "sim_rt.h"
#include "sim_common.h"

#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static constexpr char TAG[] = "counter";

static int32_t counter_state[4096 / sizeof(int32_t)];

static const char      *g_config;
static char             g_priv[512];
static int              g_id;
static tbft_replica_t  *g_replica;          /* set once Byz_init_replica returned */
static atomic_bool      g_ready;
static uint64_t         g_exec_calls;

/* Same behaviour as exec_cb() in examples/counter/main/main.c; the sim only
 * adds the SIM_EXEC trace line. */
static int exec_cb(Byz_req *in, Byz_rep *out, Byz_buffer *ndet, int cid, bool ro)
{
    (void)ndet;
    if (in->size < BYZ_REQ_HEADER_SIZE) return -1;
    const tbft_request_rep_t *hdr = (const tbft_request_rep_t *)in->contents;
    const uint8_t *cmd = (const uint8_t *)in->contents + BYZ_REQ_HEADER_SIZE;
    sim_counter_rep_t *rep = (sim_counter_rep_t *)out->contents;
    out->size = sizeof(sim_counter_rep_t);

    uint8_t op = cmd[0];
    uint64_t token = 0;
    if (hdr->command_size >= (int32_t)sizeof(sim_counter_req_t)) {
        memcpy(&token, cmd + offsetof(sim_counter_req_t, token), sizeof(token));
    }

    if (op == 0) {
        Byz_modify(&counter_state[0], sizeof(int32_t));
        counter_state[0]++;
        rep->value = counter_state[0];
        rep->status = 0;
        ESP_LOGI(TAG, "Incremented counter to %ld", (long)counter_state[0]);
    } else if (op == 1) {
        rep->value = counter_state[0];
        rep->status = 0;
        ESP_LOGI(TAG, "Read counter = %ld", (long)counter_state[0]);
    } else {
        rep->value = 0;
        rep->status = -1;
    }

    g_exec_calls++;
    /* exec_cb runs inside tbft_replica_execute_committed(), which sets
     * last_executed = n only after the callback: the seqno is last+1. */
    long long seqno = g_replica ? (long long)(g_replica->last_executed + 1) : -1;
    long long view  = g_replica ? (long long)g_replica->node.view : -1;
    sim_printf("SIM_EXEC id=%d seqno=%lld view=%lld value=%d cid=%d rid=%llu "
               "token=%016llx op=%u ro=%d",
               g_id, seqno, view, (int)rep->value, cid,
               (unsigned long long)(hdr->rid & 0xffffffffffffULL),
               (unsigned long long)token, (unsigned)op, ro ? 1 : 0);
    return 0;
}

static void replica_task(void *arg)
{
    int local_id = (int)(intptr_t)arg;
    ESP_LOGI(TAG, "Replica, local_id=%d", local_id);
    counter_state[0] = 0;

    int ret = Byz_init_replica(g_config, g_priv, local_id,
                               counter_state, sizeof(counter_state),
                               exec_cb, nullptr, 0, nullptr, 0);
    if (ret != 0) {
        sim_fatal_reboot("Byz_init_replica failed");
    }
    /* The replica owns an esp_timer named "tbft_vtimer" whose arg is
     * &replica->vtimer (tbft_itimer_init); that locates the static
     * tbft_replica_t inside tbft_libbyz.c without changing src/. */
    void *vt = sim_esp_timer_find_arg("tbft_vtimer");
    if (vt) {
        g_replica = (tbft_replica_t *)((char *)vt - offsetof(tbft_replica_t, vtimer));
    }
    atomic_store(&g_ready, true);

    ESP_LOGI(TAG, "Replica running...");
    Byz_replica_run();
}

static void print_status(bool final)
{
    static int32_t snap[sizeof(counter_state) / sizeof(int32_t)];
    long long view = -1, last_exec = -1, last_stable = -1, last_prep = -1, seqno = -1;
    long long fetch_seqno = -1, target_view = -1;
    int keys = 0, in_fetch = 0, vc_in_progress = 0, rq = 0, running = 0, primary = -1;
    unsigned long executed = 0, vc_sent = 0, new_view = 0, fetch = 0, commit_resent = 0;
    bool ready = atomic_load(&g_ready) && g_replica;

    sim_gil_lock();
    unsigned long long exec_calls = g_exec_calls;
    if (ready) {
        tbft_replica_t *r = g_replica;
        view = r->node.view;
        primary = (int)(r->node.view % r->node.num_replicas);
        last_exec = r->last_executed;
        last_stable = r->last_stable;
        last_prep = r->last_prepared;
        seqno = r->seqno;
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            if (r->node.principals[i] && r->node.principals[i]->keys_fresh) keys++;
        }
        in_fetch = r->state.in_fetch;
        fetch_seqno = r->state.fetch_seqno;
        vc_in_progress = r->vi.in_progress;
        target_view = r->vi.target_view;
        rq = r->rqueue.count;
        running = r->running;
        executed = r->stat_executed;
        vc_sent = r->stat_view_change_sent;
        new_view = r->stat_new_view;
        fetch = r->stat_fetch_started;
        commit_resent = r->stat_commit_resent;
    }
    memcpy(snap, counter_state, sizeof(snap));
    if (final && ready) tbft_replica_print_stats(g_replica);
    sim_gil_unlock();

    uint8_t dg[32];
    sim_sha256(snap, sizeof(snap), dg);
    char hex[65];
    sim_hex(dg, sizeof(dg), hex);
    sim_net_stats_t ns;
    sim_net_get_stats(&ns);

    sim_printf("SIM_REPLICA id=%d view=%lld last_exec=%lld last_stable=%lld "
               "counter=%d state_digest=%s ready=%d running=%d primary=%d "
               "last_prepared=%lld seqno=%lld keys=%d in_fetch=%d fetch_seqno=%lld "
               "vc_in_progress=%d target_view=%lld rqueue=%d executed=%lu "
               "exec_calls=%llu vc_sent=%lu new_view=%lu fetch=%lu commit_resent=%lu "
               "net_sent=%llu net_lost=%llu final=%d",
               g_id, view, last_exec, last_stable, (int)snap[0], hex,
               ready ? 1 : 0, running, primary, last_prep, seqno, keys, in_fetch,
               fetch_seqno, vc_in_progress, target_view, rq, executed,
               exec_calls, vc_sent, new_view, fetch,
               commit_resent, (unsigned long long)ns.sent,
               (unsigned long long)ns.dropped_loss, final ? 1 : 0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <id> <config_file> [priv_key_file]\n", argv[0]);
        return 2;
    }
    g_id = atoi(argv[1]);
    g_config = argv[2];
    if (argc > 3) {
        snprintf(g_priv, sizeof(g_priv), "%s", argv[3]);
    } else {
        sim_default_priv_path(g_config, g_id, g_priv, sizeof(g_priv));
    }
    sim_rt_set_node(g_id, "replica");

    char ip[64];
    int num_nodes = 0, num_replicas = 0;
    if (sim_cfg_lookup(g_config, g_id, ip, sizeof(ip), &num_nodes, &num_replicas) != 0 ||
        sim_net_set_local_ip(ip) != 0) {
        fprintf(stderr, "node %d not found in %s\n", g_id, g_config);
        return 2;
    }
    sim_block_term_signals();
    sim_printf("SIM_BOOT role=replica id=%d incarnation=%d pid=%d ip=%s nodes=%d replicas=%d "
               "hz=%d idle_wait_ms=%d window=%d ckpt_interval=%d auth=%s loss=%s",
               g_id, sim_rt_incarnation(), (int)getpid(), ip, num_nodes, num_replicas,
               CONFIG_FREERTOS_HZ, CONFIG_TBFT_IDLE_WAIT_MS, TBFT_WINDOW_SIZE,
               TBFT_CHECKPOINT_INTERVAL,
               getenv("SIM_AUTH") ? getenv("SIM_AUTH") : "strict",
               getenv("SIM_LOSS") ? getenv("SIM_LOSS") : "0");

    xTaskCreate(replica_task, "replica_task", 8192, (void *)(intptr_t)g_id, 5, nullptr);

    int period_ms = sim_rt_env_int("SIM_STATUS_MS", 1000);
    for (;;) {
        int sig = sim_wait_term_signal(period_ms);
        if (sig) {
            print_status(true);
            sim_printf("SIM_EXIT signal=%d", sig);
            sim_log_flush();
            _exit(0);
        }
        print_status(false);
    }
}
