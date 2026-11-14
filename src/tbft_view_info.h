#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_special_region.h"

/* --------------------------------------------------------------------------
 * View_info — manages the view-change protocol state (section 9).
 *
 * Tracks:
 *  - The newest View_change of each replica (stored in the special region)
 *  - Which of them are for the target view, and whether there are 2f+1
 *    (enough for the target view's primary to send New_view)
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
 * Aim at @p target_view, counting the stored View_change messages (the
 * newest one of each replica) that are for exactly that view.
 */
void tbft_vi_set_target(tbft_view_info_t *vi, tbft_view_t target_view);

/* --------------------------------------------------------------------------
 * View-change collection
 * -------------------------------------------------------------------------- */

/**
 * Store a View_change message from @p sender_id unless an older one
 * replaces a newer: each replica keeps only its newest View_change.
 * @return true if the message is for the current target view
 */
bool tbft_vi_collect_vc(tbft_view_info_t *vi,
                        tbft_node_id_t sender_id,
                        const void *msg, int msg_len);

/**
 * Returns true when 2f+1 view-change messages have been received.
 */
bool tbft_vi_has_quorum(const tbft_view_info_t *vi);

