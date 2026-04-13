#include "tbft_node.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "tbft_node";

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

int tbft_node_init(tbft_node_t *node, tbft_node_id_t node_id,
                   int f, int num_nodes,
                   const char *mcast_ip, int64_t auth_timeout_us,
                   uint16_t port)
{
    memset(node, 0, sizeof(*node));

    node->node_id       = node_id;
    node->max_faulty    = f;
    node->num_replicas  = 3 * f + 1;
    node->threshold     = 2 * f + 1;
    node->num_principals = num_nodes;
    node->view           = 0;
    node->cur_primary    = 0;
    node->auth_timeout_us = auth_timeout_us;

    /* Create transport (type selected by Kconfig) */
#if CONFIG_TBFT_TRANSPORT_ESPNOW
    tbft_transport_type_t ttype = TBFT_TRANSPORT_ESPNOW;
    ESP_LOGI(TAG, "transport: ESP-NOW");
#else
    tbft_transport_type_t ttype = TBFT_TRANSPORT_UDP;
    ESP_LOGI(TAG, "transport: UDP (mcast=%s)", mcast_ip ? mcast_ip : "off");
#endif

    if (tbft_transport_create(&node->transport, ttype, num_nodes,
                              mcast_ip, port) != 0) {
        ESP_LOGE(TAG, "failed to create transport");
        return -1;
    }

    /* Initialise authentication freshness timer */
    esp_err_t err = tbft_itimer_init(&node->atimer, NULL, node, "tbft_auth");
    if (err != ESP_OK) {
        tbft_transport_free(node->transport);
        node->transport = NULL;
        return -1;
    }

    ESP_LOGI(TAG, "node %d init: f=%d n=%d quorum=%d",
             node_id, f, node->num_replicas, node->threshold);
    return 0;
}

void tbft_node_free(tbft_node_t *node)
{
    tbft_itimer_free(&node->atimer);
    tbft_transport_free(node->transport);
    node->transport = NULL;
    for (int i = 0; i < node->num_principals; i++) {
        if (node->principals[i]) {
            tbft_principal_free(node->principals[i]);
            node->principals[i] = NULL;
        }
    }
}

/* --------------------------------------------------------------------------
 * Network I/O — delegated to transport layer
 * -------------------------------------------------------------------------- */

int tbft_node_send(tbft_node_t *node, const void *buf, size_t len,
                   tbft_node_id_t dest)
{
    if (!node->transport) return -1;
    return tbft_transport_send(node->transport, buf, len, dest);
}

int tbft_node_recv(tbft_node_t *node, void *buf, tbft_node_id_t *src_id)
{
    if (!node->transport) return -1;
    int n = tbft_transport_recv(node->transport, buf,
                                TBFT_MAX_MESSAGE_SIZE, src_id);
    if (n < (int)sizeof(tbft_msg_hdr_t)) {
        return 0; /* truncated or nothing available */
    }
    return n;
}

/* --------------------------------------------------------------------------
 * Authenticator generation / verification
 * -------------------------------------------------------------------------- */

void tbft_node_gen_auth(tbft_node_t *node, const void *msg, size_t msg_len,
                        tbft_auth_t *auth)
{
    int slot = 0;
    for (int i = 0; i < node->num_replicas; i++) {
        if (i == node->node_id) continue;
        tbft_principal_t *p = node->principals[i];
        if (p) {
            tbft_principal_gen_mac_out(p, msg, msg_len, &auth->slots[slot]);
        } else {
            memset(&auth->slots[slot], 0, sizeof(auth->slots[slot]));
        }
        slot++;
    }
}

bool tbft_node_verify_auth(tbft_node_t *node, tbft_node_id_t sender_id,
                           const void *msg, size_t msg_len,
                           const tbft_mac_t *mac)
{
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_mac_in(p, msg, msg_len, mac);
}

/* --------------------------------------------------------------------------
 * RSA signature path
 * -------------------------------------------------------------------------- */

int tbft_node_gen_sig(tbft_node_t *node, const void *msg, size_t msg_len,
                      tbft_sig_t *sig)
{
    if (!node->local_principal) return -1;
    return tbft_principal_sign(node->local_principal, msg, msg_len, sig);
}

bool tbft_node_verify_sig(tbft_node_t *node, tbft_node_id_t sender_id,
                          const void *msg, size_t msg_len,
                          const tbft_sig_t *sig)
{
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_sig(p, msg, msg_len, sig);
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

tbft_req_id_t tbft_node_new_rid(tbft_node_t *node)
{
    node->rid_counter++;
    return ((tbft_req_id_t)(uint64_t)node->node_id << 48) | node->rid_counter;
}
