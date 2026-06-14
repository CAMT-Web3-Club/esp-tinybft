#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * special_region — static storage for all non-hot-path protocol messages
 * (section 7):
 *
 *  - View_change:     one per replica
 *  - View_change_ack: one per (replica × replica) pair
 *  - New_view:        one
 *  - New_key:         one
 *  - Request:         one per client
 *  - Reply:           one per client
 * -------------------------------------------------------------------------- */

/* Size bounds for variable-length messages */
#define TBFT_VC_MSG_MAX_SIZE   TBFT_MAX_MESSAGE_SIZE
#define TBFT_NV_MSG_MAX_SIZE   TBFT_MAX_MESSAGE_SIZE
#define TBFT_NK_MSG_MAX_SIZE   (sizeof(tbft_new_key_rep_t) + TBFT_SIG_SIZE)
#define TBFT_REQ_MSG_MAX_SIZE  TBFT_MAX_MESSAGE_SIZE
#define TBFT_REP_MSG_MAX_SIZE  (sizeof(tbft_reply_rep_t) + TBFT_MAX_REPLY_SIZE \
                                + TBFT_SIG_SIZE)

/** Storage slot for a single variably-sized message */
#define TBFT_SR_SLOT(max_size)  \
    struct { uint8_t buf[(max_size)]; int len; bool valid; }

typedef struct {
    /* View-change messages: one per replica */
    TBFT_SR_SLOT(TBFT_VC_MSG_MAX_SIZE)   view_change[TBFT_MAX_NUM_REPLICAS];

    /* View-change acks: [sender][vc_sender] */
    TBFT_SR_SLOT(sizeof(tbft_vc_ack_rep_t))
        vc_ack[TBFT_MAX_NUM_REPLICAS][TBFT_MAX_NUM_REPLICAS];

    /* New-view message (one active at a time) */
    TBFT_SR_SLOT(TBFT_NV_MSG_MAX_SIZE)   new_view;

    /* New-key message */
    TBFT_SR_SLOT(TBFT_NK_MSG_MAX_SIZE)   new_key;

    /* Request cache: one per client */
    TBFT_SR_SLOT(TBFT_REQ_MSG_MAX_SIZE)  request[TBFT_MAX_NUM_CLIENTS];

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

void tbft_sr_clear_vc(tbft_special_region_t *sr);

/* --------------------------------------------------------------------------
 * View-change ack
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_vc_ack(tbft_special_region_t *sr,
                          tbft_node_id_t sender, tbft_node_id_t vc_sender,
                          const void *msg, int msg_len);

/* --------------------------------------------------------------------------
 * New-view
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_nv(tbft_special_region_t *sr,
                      const void *msg, int msg_len);

const uint8_t *tbft_sr_load_nv(const tbft_special_region_t *sr, int *len_out);

/* --------------------------------------------------------------------------
 * Request / Reply cache
 * -------------------------------------------------------------------------- */

bool tbft_sr_store_request(tbft_special_region_t *sr,
                           int client_idx,
                           const void *msg, int msg_len);

const uint8_t *tbft_sr_load_request(const tbft_special_region_t *sr,
                                    int client_idx, int *len_out);

bool tbft_sr_store_reply(tbft_special_region_t *sr,
                         int client_idx,
                         const void *msg, int msg_len);

const uint8_t *tbft_sr_load_reply(const tbft_special_region_t *sr,
                                  int client_idx, int *len_out);
