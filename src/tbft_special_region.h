#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * special_region — static storage for all non-hot-path protocol messages
 * (section 7):
 *
 *  - View_change: the newest one of each replica
 *  - New_view:    the one that started the current view (re-sent to
 *                 replicas still in an older view)
 *  - Reply:       the last signed reply to each client (re-sent when the
 *                 client retransmits its request)
 * -------------------------------------------------------------------------- */

/* Size bounds of the stored messages: the largest each can be, body aligned
 * to 8 bytes plus the RSA signature. */
static constexpr int TBFT_VC_MSG_MAX_SIZE =
    (int)((sizeof(tbft_view_change_rep_t) + sizeof(tbft_vc_ckpt_t) +
           TBFT_WINDOW_SIZE * sizeof(tbft_vc_req_info_t) + 7) / 8 * 8 + TBFT_SIG_SIZE);
static constexpr int TBFT_NV_MSG_MAX_SIZE =
    (int)((sizeof(tbft_new_view_rep_t) +
           TBFT_WINDOW_SIZE * sizeof(tbft_vc_req_info_t) + 7) / 8 * 8 + TBFT_SIG_SIZE);
static constexpr size_t TBFT_REP_MSG_MAX_SIZE =
    (sizeof(tbft_reply_rep_t) + TBFT_MAX_REPLY_SIZE + 7) / 8 * 8 + TBFT_SIG_SIZE;

static_assert(TBFT_VC_MSG_MAX_SIZE <= TBFT_MAX_MESSAGE_SIZE &&
              TBFT_NV_MSG_MAX_SIZE <= TBFT_MAX_MESSAGE_SIZE,
              "TBFT_MAX_MESSAGE_SIZE is too small for a View_change / New_view that "
              "covers TBFT_WINDOW_SIZE sequence numbers: raise it or lower the window");

/** Storage slot for a single variably-sized message */
#define TBFT_SR_SLOT(max_size)  \
    struct { uint8_t buf[(max_size)]; int len; bool valid; }

typedef struct {
    /* View-change messages: one per replica */
    TBFT_SR_SLOT(TBFT_VC_MSG_MAX_SIZE)   view_change[TBFT_MAX_NUM_REPLICAS];

    /* New-view message (one active at a time) */
    TBFT_SR_SLOT(TBFT_NV_MSG_MAX_SIZE)   new_view;

    /* Reply cache: last reply per client */
    TBFT_SR_SLOT(TBFT_REP_MSG_MAX_SIZE)  reply[TBFT_MAX_NUM_CLIENTS];

    int num_replicas;
    int num_clients;
} tbft_special_region_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_sr_init(tbft_special_region_t *sr,
                  int num_replicas, int num_clients);

/* --------------------------------------------------------------------------
 * View-change
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_vc(tbft_special_region_t *sr,
                      tbft_node_id_t from_replica,
                      const void *msg, int msg_len);

const uint8_t *tbft_sr_load_vc(const tbft_special_region_t *sr,
                                tbft_node_id_t from_replica, int *len_out);

/* --------------------------------------------------------------------------
 * New-view
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_nv(tbft_special_region_t *sr,
                      const void *msg, int msg_len);

const uint8_t *tbft_sr_load_nv(const tbft_special_region_t *sr, int *len_out);

/* --------------------------------------------------------------------------
 * Reply cache
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_reply(tbft_special_region_t *sr,
                         int client_idx,
                         const void *msg, int msg_len);

const uint8_t *tbft_sr_load_reply(const tbft_special_region_t *sr,
                                  int client_idx, int *len_out);
