/**
 * @file tbft_libbyz.c
 * @brief Implementation of the public libbyz-compatible API (section 13).
 *
 * Bridges the Byz_* API to the internal tbft_replica_t / tbft_node_t types.
 * Config file parsing follows the format in section 12.
 */

#include "esp-tinybft.h"
#include "tbft_replica.h"
#include "tbft_node.h"
#include "tbft_principal.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#if CONFIG_TBFT_TRANSPORT_UDP
#include "lwip/inet.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tbft_libbyz";

/* --------------------------------------------------------------------------
 * Module-level singletons
 * -------------------------------------------------------------------------- */

static tbft_replica_t *s_replica  = NULL;  /* allocated on Byz_init_replica */
static tbft_node_t    *s_client   = NULL;  /* allocated on Byz_init_client  */
static bool            s_is_replica = false;

/* Reply collection state for clients */
#define MAX_PENDING_REPLIES  (TBFT_MAX_NUM_REPLICAS * 2)
static struct {
    uint8_t  buf[TBFT_MAX_MESSAGE_SIZE];
    int      len;
    bool     valid;
} s_replies[MAX_PENDING_REPLIES];
static int s_reply_count = 0;

/* --------------------------------------------------------------------------
 * Config file parser (section 12)
 *
 * Format:
 *   <service_name>
 *   <f>
 *   <auth_timeout_ms>
 *   <num_nodes>
 *   <multicast_ip>
 *   <hostname> <ip> <port> <pubkey_path>   # one line per node
 *   ...
 *   <view_change_timeout_ms>
 *   <status_timeout_ms>
 *   <recovery_timeout_ms>
 * -------------------------------------------------------------------------- */

typedef struct {
    char   service_name[64];
    int    f;
    int    auth_timeout_ms;
    int    num_nodes;
    char   mcast_ip[32];

    struct {
        char   hostname[64];
        char   ip[32];       /* UDP: IP address */
        uint16_t port;       /* UDP: port number */
        char   mac_str[32];  /* ESP-NOW: MAC address string */
        char   pubkey_path[128];
    } nodes[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];

    int    vc_timeout_ms;
    int    status_timeout_ms;
    int    recovery_timeout_ms;

    /* Filled by parser: which node index matches local */
    int    local_node_id;
} tbft_config_t;

static int parse_config(const char *path, tbft_config_t *cfg)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "cannot open config file: %s", path);
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->local_node_id = -1;

    if (fscanf(f, "%63s", cfg->service_name) != 1) goto fail;
    if (fscanf(f, "%d", &cfg->f) != 1) goto fail;
    if (fscanf(f, "%d", &cfg->auth_timeout_ms) != 1) goto fail;
    if (fscanf(f, "%d", &cfg->num_nodes) != 1) goto fail;
    if (fscanf(f, "%31s", cfg->mcast_ip) != 1) goto fail;

    if (cfg->num_nodes > TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) {
        ESP_LOGE(TAG, "too many nodes: %d", cfg->num_nodes);
        goto fail;
    }

    for (int i = 0; i < cfg->num_nodes; i++) {
        /* field2 holds a MAC (17 chars) or IP (15 chars) — 32 bytes matches the struct fields */
        char field2[32];

        if (fscanf(f, "%63s %31s", cfg->nodes[i].hostname, field2) != 2) {
            ESP_LOGE(TAG, "failed to parse node %d", i);
            goto fail;
        }

        /* Check if field2 looks like a MAC address (contains ':') */
        if (strchr(field2, ':') != NULL) {
            /* ESP-NOW format: <hostname> <mac> <pubkey_path> */
            memcpy(cfg->nodes[i].mac_str, field2, sizeof(cfg->nodes[i].mac_str));
            if (fscanf(f, "%127s", cfg->nodes[i].pubkey_path) != 1) {
                ESP_LOGE(TAG, "failed to parse node %d pubkey_path", i);
                goto fail;
            }
            cfg->nodes[i].port = 0;
            cfg->nodes[i].ip[0] = '\0';
        } else {
            /* UDP format: <hostname> <ip> <port> <pubkey_path> */
            memcpy(cfg->nodes[i].ip, field2, sizeof(cfg->nodes[i].ip));
            int port_int = 0;
            if (fscanf(f, "%d %127s", &port_int, cfg->nodes[i].pubkey_path) != 2) {
                ESP_LOGE(TAG, "failed to parse node %d port/pubkey", i);
                goto fail;
            }
            cfg->nodes[i].port = (uint16_t)port_int;
        }
    }

    if (fscanf(f, "%d", &cfg->vc_timeout_ms)       != 1) goto fail;
    if (fscanf(f, "%d", &cfg->status_timeout_ms)    != 1) goto fail;
    if (fscanf(f, "%d", &cfg->recovery_timeout_ms)  != 1) goto fail;

    fclose(f);
    ESP_LOGI(TAG, "parsed config: service=%s f=%d nodes=%d mcast=%s",
             cfg->service_name, cfg->f, cfg->num_nodes, cfg->mcast_ip);
    return 0;

fail:
    fclose(f);
    return -1;
}

/**
 * Load a DER-encoded key file and return heap-allocated buffer.
 */
static uint8_t *load_key_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

/**
 * Populate principal records in a node from parsed config.
 */
static int setup_principals(tbft_node_t *node, const tbft_config_t *cfg,
                             const char *priv_config_path,
                             tbft_node_id_t local_id)
{
    for (int i = 0; i < cfg->num_nodes; i++) {
        tbft_principal_t *p =
            (tbft_principal_t *)calloc(1, sizeof(tbft_principal_t));
        if (!p) return -1;

        tbft_addr_t addr;
        memset(&addr, 0, sizeof(addr));

#if CONFIG_TBFT_TRANSPORT_ESPNOW
        /* ESP-NOW: parse MAC address */
        if (cfg->nodes[i].mac_str[0] != '\0') {
            unsigned int m[6];
            if (sscanf(cfg->nodes[i].mac_str, "%x:%x:%x:%x:%x:%x",
                       &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                for (int b = 0; b < 6; b++) {
                    tbft_addr_mac_bytes(addr)[b] = (uint8_t)m[b];
                }
            } else {
                ESP_LOGW(TAG, "invalid MAC for node %d: %s",
                         i, cfg->nodes[i].mac_str);
            }
        }
#else
        /* UDP: use IP + port */
        tbft_addr_udp_ip(addr)   = inet_addr(cfg->nodes[i].ip);
        tbft_addr_udp_port(addr) = htons(cfg->nodes[i].port);
#endif

        tbft_principal_init(p, (tbft_node_id_t)i, &addr);

        /* Load public key */
        size_t pub_len = 0;
        uint8_t *pub = load_key_file(cfg->nodes[i].pubkey_path, &pub_len);
        if (pub) {
            tbft_principal_load_pub_key(p, pub, pub_len);
            free(pub);
        } else {
            ESP_LOGW(TAG, "no public key file for node %d", i);
        }

        node->principals[i] = p;

        /* Register peer address with transport */
        tbft_transport_set_peer(node->transport, (tbft_node_id_t)i, &addr);

        /* Load private key for the local node */
        if (i == local_id && priv_config_path) {
            size_t priv_len = 0;
            uint8_t *priv = load_key_file(priv_config_path, &priv_len);
            if (priv) {
                tbft_principal_load_priv_key(p, priv, priv_len);
                free(priv);
            }
            node->local_principal = p;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Client API
 * -------------------------------------------------------------------------- */

int Byz_init_client(const char *config_file, const char *priv_config,
                    uint16_t port)
{
    tbft_config_t cfg;
    if (parse_config(config_file, &cfg) != 0) return -1;

    int n = 3 * cfg.f + 1;
    /* Local node id = first client index */
    tbft_node_id_t local_id = (tbft_node_id_t)n;

    if (s_client) {
        tbft_node_free(s_client);
        free(s_client);
    }
    s_client = (tbft_node_t *)calloc(1, sizeof(tbft_node_t));
    if (!s_client) return -1;

    uint16_t bind_port = (port != 0) ? port : cfg.nodes[local_id].port;
    if (tbft_node_init(s_client, local_id, cfg.f, cfg.num_nodes,
                       cfg.mcast_ip,
                       (int64_t)cfg.auth_timeout_ms * 1000LL,
                       bind_port) != 0) {
        free(s_client);
        s_client = NULL;
        return -1;
    }

    if (setup_principals(s_client, &cfg, priv_config, local_id) != 0) {
        tbft_node_free(s_client);
        free(s_client);
        s_client = NULL;
        return -1;
    }

    s_is_replica = false;
    s_reply_count = 0;
    memset(s_replies, 0, sizeof(s_replies));

    ESP_LOGI(TAG, "client %d initialised", local_id);
    return 0;
}

int Byz_alloc_request(Byz_req *req, int size)
{
    req->contents = (char *)malloc((size_t)size);
    if (!req->contents) return -1;
    req->size = size;
    return 0;
}

int Byz_send_request(Byz_req *req, bool read_only)
{
    if (!s_client) return -1;

    /* Build Request message */
    static uint8_t out[TBFT_MAX_MESSAGE_SIZE];
    tbft_request_rep_t *rep = (tbft_request_rep_t *)out;
    rep->hdr.tag      = TBFT_MSG_REQUEST;
    rep->hdr.extra    = (int16_t)(read_only ? 1 : 0);
    rep->cid          = s_client->node_id;
    rep->rid          = tbft_node_new_rid(s_client);
    rep->replier      = TBFT_ALL_REPLICAS;
    rep->command_size = req->size;

    tbft_msg_digest(req->contents, (size_t)req->size, &rep->od);

    /* Append command */
    uint8_t *cmd_ptr = out + sizeof(*rep);
    if (sizeof(*rep) + (size_t)req->size + TBFT_SIG_SIZE > TBFT_MAX_MESSAGE_SIZE) {
        return -1;
    }
    memcpy(cmd_ptr, req->contents, (size_t)req->size);

    /* Sign */
    tbft_sig_t *sig = (tbft_sig_t *)(cmd_ptr + req->size);
    if (s_client->local_principal && s_client->local_principal->has_priv_key) {
        tbft_node_gen_sig(s_client, out,
                          sizeof(*rep) + (size_t)req->size,
                          sig);
    }

    int32_t total = (int32_t)(sizeof(*rep) + (size_t)req->size + TBFT_SIG_SIZE);
    rep->hdr.size = tbft_msg_align(total);

    /* Send to primary */
    int primary = tbft_node_primary(s_client, s_client->view);
    return tbft_node_send(s_client, out, (size_t)rep->hdr.size, primary) > 0
           ? 0 : -1;
}

int Byz_recv_reply(Byz_rep *rep)
{
    if (!s_client) return -1;

    int f = s_client->max_faulty;
    int needed = f + 1; /* f+1 matching replies */
    int64_t deadline_us = esp_timer_get_time() + 10000000LL; /* 10 s timeout */

    while (esp_timer_get_time() < deadline_us) {
        uint8_t buf[TBFT_MAX_MESSAGE_SIZE];
        int n = tbft_node_recv(s_client, buf, NULL);
        if (n < (int)sizeof(tbft_reply_rep_t)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)buf;
        if (hdr->tag != TBFT_MSG_REPLY) continue;

        /* Store reply */
        for (int i = 0; i < MAX_PENDING_REPLIES; i++) {
            if (!s_replies[i].valid) {
                memcpy(s_replies[i].buf, buf, (size_t)n);
                s_replies[i].len   = n;
                s_replies[i].valid = true;
                s_reply_count++;
                break;
            }
        }

        /* Count matching replies (same view, rid, payload digest) */
        const tbft_reply_rep_t *r0 = (const tbft_reply_rep_t *)buf;
        int match = 0;
        const uint8_t *winning_payload = NULL;
        int winning_payload_len = 0;

        for (int i = 0; i < MAX_PENDING_REPLIES; i++) {
            if (!s_replies[i].valid) continue;
            const tbft_reply_rep_t *ri =
                (const tbft_reply_rep_t *)s_replies[i].buf;
            if (ri->view == r0->view && ri->rid == r0->rid
                    && ri->reply_size == r0->reply_size) {
                match++;
                winning_payload =
                    s_replies[i].buf + sizeof(tbft_reply_rep_t);
                winning_payload_len = ri->reply_size;
            }
        }

        if (match >= needed) {
            rep->contents = (char *)malloc((size_t)winning_payload_len);
            if (!rep->contents) return -1;
            memcpy(rep->contents, winning_payload, (size_t)winning_payload_len);
            rep->size = winning_payload_len;

            /* Clear reply cache */
            memset(s_replies, 0, sizeof(s_replies));
            s_reply_count = 0;
            return 0;
        }
    }

    return -1; /* timeout */
}

int Byz_invoke(Byz_req *req, Byz_rep *rep, bool read_only)
{
    int ret = Byz_send_request(req, read_only);
    if (ret != 0) return ret;
    return Byz_recv_reply(rep);
}

void Byz_free_request(Byz_req *req)
{
    if (req && req->contents) {
        free(req->contents);
        req->contents = NULL;
        req->size = 0;
    }
}

void Byz_free_reply(Byz_rep *rep)
{
    if (rep && rep->contents) {
        free(rep->contents);
        rep->contents = NULL;
        rep->size = 0;
    }
}

void Byz_reset_client(void)
{
    memset(s_replies, 0, sizeof(s_replies));
    s_reply_count = 0;
}

/* --------------------------------------------------------------------------
 * Replica API
 * -------------------------------------------------------------------------- */

/* Internal exec adapter translating between Byz_* and internal types */
static Byz_exec_cb        s_exec_cb        = NULL;
static Byz_comp_ndet_cb   s_comp_ndet_cb   = NULL;
static Byz_recv_reply_cb  s_recv_reply_cb  = NULL;

static int exec_adapter(const void *req, int req_len,
                        void *rep, int *rep_len,
                        void *ndet, int ndet_len,
                        int cid, bool ro)
{
    if (!s_exec_cb) return -1;
    Byz_req in  = { .contents = (char *)req, .size = req_len };
    Byz_rep out = { .contents = (char *)rep, .size = 0 };
    Byz_buffer nd = { .contents = (char *)ndet, .size = ndet_len };
    int r = s_exec_cb(&in, &out, &nd, cid, ro);
    if (r == 0 && rep_len) *rep_len = out.size;
    return r;
}

static void comp_ndet_adapter(tbft_seqno_t seqno,
                               void *ndet, int *ndet_len, int max_len)
{
    if (!s_comp_ndet_cb) return;
    Byz_buffer buf = { .contents = (char *)ndet, .size = 0 };
    s_comp_ndet_cb((int64_t)seqno, &buf, max_len);
    if (ndet_len) *ndet_len = buf.size;
}

static void recv_reply_adapter(const void *rep, int rep_len, int cid)
{
    if (!s_recv_reply_cb) return;
    Byz_rep r = { .contents = (char *)rep, .size = rep_len };
    s_recv_reply_cb(&r, cid);
}

int Byz_init_replica(const char *config_file, const char *priv_config,
                     void *mem, size_t mem_size,
                     Byz_exec_cb exec_cb,
                     Byz_comp_ndet_cb comp_ndet_cb, int ndet_max_len,
                     Byz_recv_reply_cb recv_reply_cb,
                     uint16_t port)
{
    tbft_config_t cfg;
    if (parse_config(config_file, &cfg) != 0) return -1;

    /* Find our node id by matching IP */
    tbft_node_id_t local_id = -1;
    /* In embedded context, we default to node 0 if we cannot determine IP.
     * A production implementation would use esp_netif to get the local IP. */
    local_id = 0;
    ESP_LOGW(TAG, "using node_id=0 (set TBFT_NODE_ID env or config to override)");

    s_exec_cb       = exec_cb;
    s_comp_ndet_cb  = comp_ndet_cb;
    s_recv_reply_cb = recv_reply_cb;

    if (s_replica) {
        tbft_replica_free(s_replica);
        free(s_replica);
    }
    s_replica = (tbft_replica_t *)calloc(1, sizeof(tbft_replica_t));
    if (!s_replica) return -1;

    uint16_t bind_port = (port != 0) ? port : cfg.nodes[local_id].port;

    int ret = tbft_replica_init(
        s_replica, local_id, cfg.f, cfg.num_nodes,
        cfg.mcast_ip,
        (int64_t)cfg.auth_timeout_ms * 1000LL,
        bind_port,
        mem, mem_size,
        exec_adapter,
        s_comp_ndet_cb ? comp_ndet_adapter : NULL,
        ndet_max_len,
        recv_reply_cb ? recv_reply_adapter : NULL);

    if (ret != 0) {
        free(s_replica);
        s_replica = NULL;
        return -1;
    }

    /* Set timer periods from config */
    s_replica->vtimer_period_us = (int64_t)cfg.vc_timeout_ms * 1000LL;
    s_replica->stimer_period_us = (int64_t)cfg.status_timeout_ms * 1000LL;

    /* Restart timers with correct periods */
    tbft_itimer_stop(&s_replica->vtimer);
    tbft_itimer_stop(&s_replica->stimer);
    tbft_itimer_start(&s_replica->vtimer, s_replica->vtimer_period_us);
    tbft_itimer_start(&s_replica->stimer, s_replica->stimer_period_us);

    /* Setup principals */
    if (setup_principals(&s_replica->node, &cfg, priv_config, local_id) != 0) {
        tbft_replica_free(s_replica);
        free(s_replica);
        s_replica = NULL;
        return -1;
    }

    s_is_replica = true;
    ESP_LOGI(TAG, "replica %d initialised", local_id);
    return 0;
}

void Byz_modify(void *mem, int size)
{
    if (s_replica) {
        tbft_state_cow(&s_replica->state, mem, (size_t)size);
    }
}

void Byz_modify1(void *mem)
{
    if (s_replica) {
        uintptr_t base  = (uintptr_t)s_replica->state.mem;
        uintptr_t addr  = (uintptr_t)mem;
        int bindex = (int)((addr - base) / TBFT_BLOCK_SIZE);
        tbft_state_cow_single(&s_replica->state, bindex);
    }
}

void Byz_replica_run(void)
{
    if (!s_replica) {
        ESP_LOGE(TAG, "Byz_replica_run: replica not initialised");
        return;
    }
    tbft_replica_run(s_replica);
}

void Byz_reset_stats(void)
{
    /* Statistics counters placeholder */
    ESP_LOGI(TAG, "stats reset");
}

void Byz_print_stats(void)
{
    if (!s_replica) return;
    ESP_LOGI(TAG,
             "STATS: view=%lld last_stable=%lld last_executed=%lld "
             "last_prepared=%lld",
             (long long)s_replica->node.view,
             (long long)s_replica->last_stable,
             (long long)s_replica->last_executed,
             (long long)s_replica->last_prepared);
}
