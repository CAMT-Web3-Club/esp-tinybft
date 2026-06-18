#include "tbft_special_region.h"
#include <string.h>

void tbft_sr_init(tbft_special_region_t *sr,
                  int num_replicas, int num_clients)
{
    memset(sr, 0, sizeof(*sr));
    sr->num_replicas = num_replicas;
    sr->num_clients  = num_clients;
}

/* --------------------------------------------------------------------------
 * Generic helper macros
 * -------------------------------------------------------------------------- */

#define SR_STORE(slot, m, mlen, maxlen)                      \
    do {                                                     \
        if ((mlen) < 0 || (mlen) > (int)(maxlen)) return false;            \
        memcpy((slot).buf, (m), (size_t)(mlen));             \
        (slot).len   = (mlen);                               \
        (slot).valid = true;                                 \
        return true;                                         \
    } while (0)

#define SR_LOAD(slot, lenout)                                \
    do {                                                     \
        if (!(slot).valid) {                                 \
            if (lenout) *(lenout) = 0;                       \
            return NULL;                                     \
        }                                                    \
        if (lenout) *(lenout) = (slot).len;                  \
        return (slot).buf;                                   \
    } while (0)

/* --------------------------------------------------------------------------
 * View-change
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_vc(tbft_special_region_t *sr,
                      tbft_node_id_t from_replica,
                      const void *msg, int msg_len)
{
    if (from_replica < 0 || from_replica >= TBFT_MAX_NUM_REPLICAS)
        return false;
    SR_STORE(sr->view_change[from_replica], msg, msg_len,
             TBFT_VC_MSG_MAX_SIZE);
}

const uint8_t *tbft_sr_load_vc(const tbft_special_region_t *sr,
                                tbft_node_id_t from_replica, int *len_out)
{
    if (from_replica < 0 || from_replica >= TBFT_MAX_NUM_REPLICAS) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    SR_LOAD(sr->view_change[from_replica], len_out);
}

void tbft_sr_clear_vc(tbft_special_region_t *sr)
{
    for (int i = 0; i < TBFT_MAX_NUM_REPLICAS; i++) {
        sr->view_change[i].valid = false;
        sr->view_change[i].len   = 0;
    }
}

/* --------------------------------------------------------------------------
 * View-change ack
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_vc_ack(tbft_special_region_t *sr,
                          tbft_node_id_t sender, tbft_node_id_t vc_sender,
                          const void *msg, int msg_len)
{
    if (sender < 0 || sender >= TBFT_MAX_NUM_REPLICAS) return false;
    if (vc_sender < 0 || vc_sender >= TBFT_MAX_NUM_REPLICAS) return false;
    SR_STORE(sr->vc_ack[sender][vc_sender], msg, msg_len,
             sizeof(tbft_vc_ack_rep_t));
}

/* --------------------------------------------------------------------------
 * New-view
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_nv(tbft_special_region_t *sr,
                      const void *msg, int msg_len)
{
    SR_STORE(sr->new_view, msg, msg_len, TBFT_NV_MSG_MAX_SIZE);
}

const uint8_t *tbft_sr_load_nv(const tbft_special_region_t *sr, int *len_out)
{
    SR_LOAD(sr->new_view, len_out);
}

/* --------------------------------------------------------------------------
 * Request / Reply
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_request(tbft_special_region_t *sr,
                           int client_idx,
                           const void *msg, int msg_len)
{
    if (client_idx < 0 || client_idx >= TBFT_MAX_NUM_CLIENTS) return false;
    SR_STORE(sr->request[client_idx], msg, msg_len, TBFT_REQ_MSG_MAX_SIZE);
}

const uint8_t *tbft_sr_load_request(const tbft_special_region_t *sr,
                                    int client_idx, int *len_out)
{
    if (client_idx < 0 || client_idx >= TBFT_MAX_NUM_CLIENTS) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    SR_LOAD(sr->request[client_idx], len_out);
}

bool tbft_sr_store_reply(tbft_special_region_t *sr,
                         int client_idx,
                         const void *msg, int msg_len)
{
    if (client_idx < 0 || client_idx >= TBFT_MAX_NUM_CLIENTS) return false;
    SR_STORE(sr->reply[client_idx], msg, msg_len, TBFT_REP_MSG_MAX_SIZE);
}

const uint8_t *tbft_sr_load_reply(const tbft_special_region_t *sr,
                                  int client_idx, int *len_out)
{
    if (client_idx < 0 || client_idx >= TBFT_MAX_NUM_CLIENTS) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    SR_LOAD(sr->reply[client_idx], len_out);
}
