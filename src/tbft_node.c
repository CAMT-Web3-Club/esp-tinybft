/**
 * @file tbft_node.c
 * @brief Network I/O delegation layer — abstracts transport (UDP/ESP-NOW).
 *
 * The node layer sits between the replica (protocol logic) and the transport
 * backend. It handles:
 *   - Transport creation (type selected via Kconfig at compile time)
 *   - Message send/recv delegation to transport
 *   - Authenticator generation (HMAC) and verification (with replay check)
 *   - RSA signature generation and verification
 *   - Node ID to auth slot index mapping
 */

#include "tbft_node.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

    /* For ESP-NOW, the underlying network must be ready.  If the first
     * attempt fails (e.g. ESP-NOW not yet fully initialized), retry a few
     * times with a short delay before giving up. */
    esp_err_t transport_err = ESP_OK;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            ESP_LOGI(TAG, "transport retry attempt %d", attempt + 1);
        }
        if (tbft_transport_create(&node->transport, ttype, num_nodes,
                                  node->num_replicas,
                                  mcast_ip, port) == 0) {
            transport_err = ESP_OK;
            break;
        }
        transport_err = ESP_FAIL;
    }
    if (transport_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create transport after 5 attempts");
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

int tbft_node_recv(tbft_node_t *node, void *buf, size_t buf_len, tbft_node_id_t *src_id)
{
    if (!node->transport) return -1;
    int n = tbft_transport_recv(node->transport, buf, buf_len, src_id);
    if (n < 0) {
        return -1; /* transport error */
    }
    if (n < (int)sizeof(tbft_msg_hdr_t)) {
        return 0; /* no message or truncated */
    }
    return n;
}

/* --------------------------------------------------------------------------
 * Authenticator generation / verification
 * -------------------------------------------------------------------------- */

void tbft_node_gen_auth(tbft_node_t *node, const void *msg, size_t msg_len,
                        tbft_auth_t *auth)
{
    /* Zero the entire authenticator first.  Callers commonly stack-allocate
     * tbft_auth_t at the natural TBFT_MAX_NUM_REPLICAS-1 size, but actual
     * num_replicas may be smaller.  Without this memset, trailing unused
     * slots would be sent with whatever stack garbage happened to be there. */
    memset(auth, 0, sizeof(*auth));

    int slot = 0;
    int expected_slots = node->num_replicas - 1; /* exclude self */
    for (int i = 0; i < node->num_replicas; i++) {
        if (i == node->node_id) continue;
        /* Guard: ensure slot index never exceeds auth->slots capacity */
        if (slot >= expected_slots) break;
        tbft_principal_t *p = node->principals[i];
        if (p) {
            /* HIGH FIX H8: Check MAC generation return value.
             * Without this, failed MAC generation produces zeroed slots,
             * causing all peers to reject the message. */
            int rc = tbft_principal_gen_mac_out(p, msg, msg_len, &auth->slots[slot]);
            if (rc != 0) {
                memset(&auth->slots[slot], 0, sizeof(auth->slots[slot]));
            }
        } else {
            memset(&auth->slots[slot], 0, sizeof(auth->slots[slot]));
        }
        slot++;
    }
}

bool tbft_node_verify_auth(tbft_node_t *node, tbft_node_id_t sender_id,
                           const void *msg, size_t msg_len,
                           const tbft_mac_t *mac, int64_t timestamp_us)
{
    if (sender_id < 0 || sender_id >= node->num_principals) return false;
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_mac_in_with_replay_check(p, msg, msg_len, mac, timestamp_us);
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
    if (sender_id < 0 || sender_id >= node->num_principals) return false;
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_sig(p, msg, msg_len, sig);
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

int tbft_node_auth_slot_index(const tbft_node_t *node, tbft_node_id_t sender_id)
{
    if (sender_id == node->node_id) return -1;
    if (sender_id > node->node_id) {
        return (int)sender_id - 1;
    }
    return (int)sender_id;
}

tbft_req_id_t tbft_node_new_rid(tbft_node_t *node)
{
    node->rid_counter++;
    return ((tbft_req_id_t)(uint64_t)node->node_id << 48) | node->rid_counter;
}
