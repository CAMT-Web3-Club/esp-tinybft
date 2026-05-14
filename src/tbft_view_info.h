#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_special_region.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * View_info — manages the view-change protocol state (section 9).
 *
 * Tracks:
 *  - Which view-change messages have been received
 *  - Whether we have enough (2f+1) to construct a New_view
 *  - Computes (min, max) seqno range for the new view
 * -------------------------------------------------------------------------- */

typedef struct {
    tbft_view_t  target_view;     /* view we're trying to install */
    int          num_replicas;
    int          threshold;       /* 2f+1 */

    /* Set when send_view_change has fired; cleared when a new view is
     * installed.  Drives the PP-suppression guard so the primary does not
     * broadcast Pre-prepares while a view-change is in flight. */
    bool         in_progress;

    /* Received view-change messages (stored in special_region) */
    tbft_special_region_t *sr;    /* back-pointer to special region */

    /* Per-replica tracking */
    bool         received[TBFT_MAX_NUM_REPLICAS];
    tbft_seqno_t last_stable[TBFT_MAX_NUM_REPLICAS]; /* ls from each vc msg */

    int          n_received;      /* count of received view-change messages */
} tbft_view_info_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_vi_init(tbft_view_info_t *vi, int num_replicas, int threshold,
                  tbft_special_region_t *sr);

/**
 * Reset for a new view-change round.
 */
void tbft_vi_reset(tbft_view_info_t *vi, tbft_view_t new_target_view);

/* --------------------------------------------------------------------------
 * View-change collection
 * -------------------------------------------------------------------------- */

/**
 * Record a received View_change message from @p sender_id.
 * @return true if the message was newly accepted
 */
bool tbft_vi_collect_vc(tbft_view_info_t *vi,
                        tbft_node_id_t sender_id,
                        const void *msg, int msg_len);

/**
 * Returns true when 2f+1 view-change messages have been received.
 */
bool tbft_vi_has_quorum(const tbft_view_info_t *vi);

/* --------------------------------------------------------------------------
 * New-view computation
 * -------------------------------------------------------------------------- */

/**
 * Compute min and max sequence numbers from the collected view-changes.
 * min = max of all last_stable values
 * max = min of (last_stable + WINDOW_SIZE) across all view-changes
 */
void tbft_vi_compute_min_max(const tbft_view_info_t *vi,
                             tbft_seqno_t *min_out, tbft_seqno_t *max_out);

/**
 * Verify that a received New_view message is consistent with the collected
 * view-change messages.
 * @return true if valid
 */
bool tbft_vi_verify_nv(const tbft_view_info_t *vi,
                       const tbft_new_view_rep_t *nv, int nv_len);

/* --------------------------------------------------------------------------
 * View-change ack
 * -------------------------------------------------------------------------- */

/**
 * Store a View_change_ack received from @p sender for vc from @p vc_sender.
 */
void tbft_vi_collect_vc_ack(tbft_view_info_t *vi,
                             tbft_node_id_t sender,
                             tbft_node_id_t vc_sender,
                             const void *msg, int msg_len);
