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
#include <ctype.h>
#include "esp_mac.h"
#if CONFIG_TBFT_TRANSPORT_ESPNOW
#include "esp_efuse.h"
#endif

static const char *TAG = "tbft_libbyz";

/* --------------------------------------------------------------------------
 * Module-level singletons
 * -------------------------------------------------------------------------- */

static tbft_replica_t *s_replica  = NULL;  /* allocated on Byz_init_replica */
static tbft_node_t    *s_client   = NULL;  /* allocated on Byz_init_client  */
static bool            s_is_replica = false;

/* Reply collection state for clients.
 * Indexed by replica id — exactly one slot per replica.  This makes any
 * "match" count necessarily a count of DISTINCT senders, which is required
 * for BFT reply agreement (f+1 matching replies from different replicas). */
static struct {
    uint8_t          buf[TBFT_MAX_MESSAGE_SIZE];
    int              len;
    bool             valid;
} s_replies[TBFT_MAX_NUM_REPLICAS];
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

    /* Validate: f must fit the static cert buffers sized at compile time */
    if (cfg->f < 1 || cfg->f > TBFT_MAX_FAULTY) {
        ESP_LOGE(TAG, "f=%d out of range [1, %d] — increase TBFT_MAX_NUM_REPLICAS",
                 cfg->f, TBFT_MAX_FAULTY);
        goto fail;
    }

    /* Validate: BFT requires n >= 3f+1 replicas; extra entries are clients */
    int min_replicas = 3 * cfg->f + 1;
    if (cfg->num_nodes < min_replicas) {
        ESP_LOGE(TAG, "num_nodes=%d < 3f+1=%d", cfg->num_nodes, min_replicas);
        goto fail;
    }
    if (cfg->num_nodes > TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) {
        ESP_LOGE(TAG, "num_nodes=%d exceeds compile-time cap %d (MAX_NUM_REPLICAS=%d + MAX_NUM_CLIENTS=%d)",
                 cfg->num_nodes,
                 TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS,
                 TBFT_MAX_NUM_REPLICAS, TBFT_MAX_NUM_CLIENTS);
        goto fail;
    }

    for (int i = 0; i < cfg->num_nodes; i++) {
        /* field2 holds a MAC (17 chars) or IP (15 chars) — 32 bytes matches the struct fields */
        char field2[32];

        if (fscanf(f, "%63s %31s", cfg->nodes[i].hostname, field2) != 2) {
            ESP_LOGE(TAG, "failed to parse node %d", i);
            goto fail;
        }

        bool is_mac = (strchr(field2, ':') != NULL);

        /* Validate: file format must match the compiled-in transport */
#if CONFIG_TBFT_TRANSPORT_UDP
        if (is_mac) {
            ESP_LOGE(TAG, "node %d has MAC address but transport is UDP — "
                          "use a UDP config file (<hostname> <ip> <port> <pubkey>)", i);
            goto fail;
        }
#elif CONFIG_TBFT_TRANSPORT_ESPNOW
        if (!is_mac) {
            ESP_LOGE(TAG, "node %d has IP address but transport is ESP-NOW — "
                          "use an ESP-NOW config file (<hostname> <mac> <pubkey>)", i);
            goto fail;
        }
#endif

        if (is_mac) {
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

#if CONFIG_TBFT_TRANSPORT_ESPNOW
    /* Auto-detect local_node_id by matching the eFuse base MAC against
     * the config entries.  Config files MUST contain base (eFuse) MACs,
     * NOT AP MACs — the ESP-NOW transport applies get_ap_mac() internally. */
    uint8_t local_mac[6];
    if (esp_efuse_mac_get_default(local_mac) == ESP_OK) {
        char local_mac_str[18];
        snprintf(local_mac_str, sizeof(local_mac_str),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 local_mac[0], local_mac[1], local_mac[2],
                 local_mac[3], local_mac[4], local_mac[5]);
        for (int i = 0; i < cfg->num_nodes; i++) {
            if (cfg->nodes[i].mac_str[0] != '\0') {
                /* Case-insensitive compare (config may use upper or lower hex) */
                bool match = true;
                for (int c = 0; c < 17; c++) {
                    if (tolower((unsigned char)cfg->nodes[i].mac_str[c]) !=
                        tolower((unsigned char)local_mac_str[c])) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    cfg->local_node_id = i;
                    ESP_LOGI(TAG, "auto-detected local_node_id=%d (MAC=%s)",
                             i, local_mac_str);
                    break;
                }
            }
        }
        if (cfg->local_node_id < 0) {
            ESP_LOGW(TAG, "local MAC %s not found in config — "
                     "ensure config contains base (eFuse) MACs, not AP MACs",
                     local_mac_str);
        }
    }
#endif

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
        if (!p) {
            /* Free already-allocated principals to avoid a leak */
            for (int j = 0; j < i; j++) {
                if (node->principals[j]) {
                    tbft_principal_free(node->principals[j]);
                    free(node->principals[j]);
                    node->principals[j] = NULL;
                }
            }
            return -1;
        }

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

        /* Load public key — failure is fatal */
        size_t pub_len = 0;
        uint8_t *pub = load_key_file(cfg->nodes[i].pubkey_path, &pub_len);
        if (!pub) {
            ESP_LOGE(TAG, "no public key file for node %d: %s", i, cfg->nodes[i].pubkey_path);
            tbft_principal_free(p);
            free(p);
            for (int j = 0; j < i; j++) {
                if (node->principals[j]) {
                    tbft_principal_free(node->principals[j]);
                    free(node->principals[j]);
                    node->principals[j] = NULL;
                }
            }
            return -1;
        }
        int pub_ret = tbft_principal_load_pub_key(p, pub, pub_len);
        free(pub);
        if (pub_ret != 0) {
            ESP_LOGE(TAG, "failed to parse public key for node %d from %s",
                     i, cfg->nodes[i].pubkey_path);
            tbft_principal_free(p);
            free(p);
            for (int j = 0; j < i; j++) {
                if (node->principals[j]) {
                    tbft_principal_free(node->principals[j]);
                    free(node->principals[j]);
                    node->principals[j] = NULL;
                }
            }
            return -1;
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

            /* Verify public/private key pair consistency */
            if (p->has_priv_key) {
                int rc = mbedtls_pk_check_pair(&p->pub_pk, &p->priv_pk);
                if (rc != 0) {
                    ESP_LOGE(TAG, "key pair mismatch for node %d: "
                             "public and private keys do not correspond "
                             "(mbedtls err=%d)", i, rc);
                    for (int j = 0; j <= i; j++) {
                        if (node->principals[j]) {
                            tbft_principal_free(node->principals[j]);
                            free(node->principals[j]);
                            node->principals[j] = NULL;
                        }
                    }
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Client API
 * -------------------------------------------------------------------------- */

int Byz_init_client(const char *config_file, const char *priv_config,
                    int local_id, uint16_t port)
{
    tbft_config_t cfg;
    if (parse_config(config_file, &cfg) != 0) return -1;

    int num_replicas = 3 * cfg.f + 1;

    /* local_id == -1: auto-select first client slot (3f+1) */
    if (local_id == -1) local_id = num_replicas;

    /* Validate: local_id must be a client slot (beyond the replicas) */
    if (local_id < num_replicas || local_id >= cfg.num_nodes) {
        ESP_LOGE(TAG, "client local_id=%d out of client range [%d, %d)",
                 local_id, num_replicas, cfg.num_nodes);
        return -1;
    }

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

    /* Build Request message on stack */
    uint8_t out[TBFT_MAX_MESSAGE_SIZE];
    tbft_request_rep_t *rep = (tbft_request_rep_t *)out;
    rep->hdr.tag      = TBFT_MSG_REQUEST;
    rep->hdr.extra    = (int16_t)(read_only ? 1 : 0);
    rep->cid          = s_client->node_id;
    rep->rid          = tbft_node_new_rid(s_client);
    rep->replier      = TBFT_ALL_REPLICAS;
    rep->command_size = req->size;

    tbft_msg_digest(req->contents, (size_t)req->size, &rep->od);

    /* Set header size and timestamp BEFORE signing — the signature covers
     * the full header (including hdr.size) so it must have its final value. */
    int32_t total = (int32_t)(sizeof(*rep) + (size_t)req->size + TBFT_SIG_SIZE);
    rep->hdr.size = tbft_msg_align(total);
    rep->hdr.timestamp_us = esp_timer_get_time();

    /* Append command */
    uint8_t *cmd_ptr = out + sizeof(*rep);
    if (sizeof(*rep) + (size_t)req->size + TBFT_SIG_SIZE > TBFT_MAX_MESSAGE_SIZE) {
        return -1;
    }
    memcpy(cmd_ptr, req->contents, (size_t)req->size);

    /* Sign */
    tbft_sig_t *sig = (tbft_sig_t *)(cmd_ptr + req->size);
    memset(sig->bytes, 0, sizeof(sig->bytes));
    if (s_client->local_principal && s_client->local_principal->has_priv_key) {
        tbft_node_gen_sig(s_client, out,
                          sizeof(*rep) + (size_t)req->size,
                          sig);
    }

    /* Broadcast to all replicas — any replica that receives the request
     * will forward it to the current primary.  Sending only to the
     * primary (based on the client's stale view) is fragile: if the
     * cluster has advanced to a different view, the client's primary
     * may no longer be the real primary. */
    return tbft_node_send(s_client, out, (size_t)rep->hdr.size,
                          TBFT_ALL_REPLICAS) > 0 ? 0 : -1;
}

int Byz_recv_reply(Byz_rep *rep)
{
    if (!s_client) return -1;

    int f = s_client->max_faulty;
    int needed = f + 1; /* f+1 matching replies from DISTINCT replicas */
    int num_replicas = s_client->num_replicas;
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

        const tbft_reply_rep_t *r0 = (const tbft_reply_rep_t *)buf;

        /* Verify RSA signature and identify sender.
         * The replica appends the sig after hdr.size; total wire = hdr.size + SIG.
         * We try each replica's public key; only the true sender's key will
         * validate the signature, giving us a reliable sender id. */
        int32_t body_sz = r0->hdr.size;
        tbft_node_id_t rep_id = -1;
        if (body_sz < (int32_t)sizeof(tbft_reply_rep_t) || body_sz > n) continue;
        if (n < body_sz + (int)sizeof(tbft_sig_t)) continue;

        const tbft_sig_t *sig =
            (const tbft_sig_t *)((const uint8_t *)buf + body_sz);
        for (int ri = 0; ri < num_replicas; ri++) {
            if (tbft_node_verify_sig(s_client, (tbft_node_id_t)ri,
                                     buf, (size_t)body_sz, sig)) {
                rep_id = (tbft_node_id_t)ri;
                break;
            }
        }
        if (rep_id < 0) {
            ESP_LOGW(TAG, "recv_reply: no valid RSA signature");
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* Drop replies for old rids that were not cleared. */
        if (s_replies[rep_id].valid) {
            const tbft_reply_rep_t *old =
                (const tbft_reply_rep_t *)s_replies[rep_id].buf;
            if (old->rid != r0->rid) {
                /* Stale cached reply from a previous request — replace. */
                s_replies[rep_id].valid = false;
                s_reply_count--;
            } else {
                /* Duplicate reply for same rid from this replica — ignore. */
                continue;
            }
        }

        /* Store — one slot per replica id guarantees distinct senders. */
        if ((size_t)n > sizeof(s_replies[rep_id].buf)) continue;
        memcpy(s_replies[rep_id].buf, buf, (size_t)n);
        s_replies[rep_id].len   = n;
        s_replies[rep_id].valid = true;
        s_reply_count++;

        /* Count matching replies (same rid + payload digest).
         * Do NOT filter on view: a replica may have moved to a later view
         * but still produce a valid reply for our request id. */
        int match = 0;
        const uint8_t *winning_payload = NULL;
        int winning_payload_len = 0;
        tbft_digest_t winning_digest;
        bool first = true;

        for (int i = 0; i < num_replicas; i++) {
            if (!s_replies[i].valid) continue;
            const tbft_reply_rep_t *ri =
                (const tbft_reply_rep_t *)s_replies[i].buf;
            if (ri->rid != r0->rid) continue;
            if (ri->reply_size != r0->reply_size) continue;

            const uint8_t *payload = s_replies[i].buf + sizeof(*ri);
            tbft_digest_t digest;
            tbft_msg_digest(payload, (size_t)ri->reply_size, &digest);

            if (first) {
                winning_payload = payload;
                winning_payload_len = ri->reply_size;
                winning_digest = digest;
                match++;
                first = false;
            } else if (memcmp(&digest, &winning_digest, sizeof(digest)) == 0) {
                match++;
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
                     int local_id,
                     void *mem, size_t mem_size,
                     Byz_exec_cb exec_cb,
                     Byz_comp_ndet_cb comp_ndet_cb, int ndet_max_len,
                     Byz_recv_reply_cb recv_reply_cb,
                     uint16_t port)
{
    tbft_config_t cfg;
    if (parse_config(config_file, &cfg) != 0) return -1;

    /* Auto-detect local_id if caller passes -1 */
    if (local_id < 0) {
        if (cfg.local_node_id >= 0) {
            local_id = cfg.local_node_id;
            ESP_LOGI(TAG, "using auto-detected local_id=%d", local_id);
        } else {
            ESP_LOGE(TAG, "local_id auto-detection failed: "
                     "local MAC not found in config. Supply local_id explicitly.");
            return -1;
        }
    }

    /* Validate local_id: must be a replica slot (0..3f) in range */
    int num_replicas = 3 * cfg.f + 1;
    if (local_id < 0 || local_id >= num_replicas) {
        ESP_LOGE(TAG, "local_id=%d out of replica range [0, %d)",
                 local_id, num_replicas);
        return -1;
    }

    s_exec_cb       = exec_cb;
    s_comp_ndet_cb  = comp_ndet_cb;
    s_recv_reply_cb = recv_reply_cb;

    if (s_replica) {
        tbft_replica_free(s_replica);
        free(s_replica);
    }
    s_replica = (tbft_replica_t *)calloc(1, sizeof(tbft_replica_t));
    if (!s_replica) {
        ESP_LOGE(TAG, "failed to allocate tbft_replica_t (size=%zu). device out of memory. reduce TBFT_WINDOW_SIZE or max nodes.", sizeof(tbft_replica_t));
        return -1;
    }

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

    /* Set timer periods from config.
     * Use a 3x grace period for the initial view-change timer to allow
     * the cluster to complete HMAC key exchange before triggering view-change. */
    s_replica->vtimer_period_us = (int64_t)cfg.vc_timeout_ms * 1000LL;
    s_replica->stimer_period_us = (int64_t)cfg.status_timeout_ms * 1000LL;

    /* Restart timers with correct periods */
    tbft_itimer_stop(&s_replica->vtimer);
    tbft_itimer_stop(&s_replica->stimer);
    tbft_itimer_start(&s_replica->vtimer, s_replica->vtimer_period_us * 3);
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
        uintptr_t end   = base + s_replica->state.mem_size;
        uintptr_t addr  = (uintptr_t)mem;
        if (addr < base || addr >= end) {
            ESP_LOGW(TAG, "Byz_modify1: address %p out of state bounds", mem);
            return;
        }
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

int Byz_detect_local_id(const char *config_file)
{
    tbft_config_t cfg;
    if (parse_config(config_file, &cfg) != 0) return -1;
    return cfg.local_node_id;
}
