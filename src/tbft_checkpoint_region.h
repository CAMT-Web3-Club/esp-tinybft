#pragma once

#include "tbft_config.h"
#include "tbft_types.h"
#include "tbft_message.h"
#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * checkpoint_region — static storage for Checkpoint messages (section 7).
 *
 * Two sub-regions:
 *  1. Normal checkpoint slots: indexed by (seqno/interval) % num_slots
 *     Each slot holds one Checkpoint message per replica (MAX_NUM_REPLICAS).
 *  2. Above-window slots: one per replica, for checkpoints from replicas
 *     that are ahead of the current window.
 * -------------------------------------------------------------------------- */

/* Raw bytes for one Checkpoint message on the wire */
#define TBFT_CKPT_MSG_SIZE  (sizeof(tbft_checkpoint_rep_t) + TBFT_AUTH_SIZE)

/** One slot: Checkpoint messages from each replica at one seqno */
typedef struct {
    tbft_seqno_t seqno;    /* explicit seqno for truncate (avoids heuristic) */
    uint8_t  msgs[TBFT_MAX_NUM_REPLICAS][TBFT_CKPT_MSG_SIZE];
    int      msg_lens[TBFT_MAX_NUM_REPLICAS];
    bool     present[TBFT_MAX_NUM_REPLICAS];

    /* Multi-candidate digest tracking.
     * Tracks up to TBFT_CERT_MAX_VALS distinct digest values independently,
     * so a Byzantine replica arriving first cannot permanently poison the slot
     * by setting an incorrect winning_digest. */
    int           n_candidates;
    tbft_digest_t cand_digests[TBFT_CERT_MAX_VALS];
    int           cand_counts[TBFT_CERT_MAX_VALS];

    /* Best-so-far: updated whenever a candidate surpasses the current leader */
    int           match_count;
    tbft_digest_t winning_digest;
} tbft_ckpt_slot_t;

/** The checkpoint region */
typedef struct {
    tbft_ckpt_slot_t  slots[TBFT_NUM_CKPT_SLOTS];
    tbft_ckpt_slot_t  above_window[TBFT_MAX_NUM_REPLICAS];
    int               num_replicas;
    int               threshold;  /* 2f+1 */
} tbft_checkpoint_region_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void tbft_cr_init(tbft_checkpoint_region_t *cr,
                  int num_replicas, int threshold);

/* --------------------------------------------------------------------------
 * Normal checkpoint slot operations
 * -------------------------------------------------------------------------- */

/**
 * Store a Checkpoint message from @p replica_id for @p seqno.
 * @return true if this message makes the slot's count reach threshold
 *         (i.e., stable checkpoint reached)
 */
bool tbft_cr_store(tbft_checkpoint_region_t *cr,
                   tbft_seqno_t seqno,
                   tbft_node_id_t replica_id,
                   const void *msg, int msg_len);

/**
 * Load the Checkpoint message from @p replica_id for @p seqno.
 * @return message bytes or NULL
 */
const uint8_t *tbft_cr_load(const tbft_checkpoint_region_t *cr,
                             tbft_seqno_t seqno,
                             tbft_node_id_t replica_id, int *len_out);

/**
 * Return the number of matching Checkpoint messages received for @p seqno.
 */
int tbft_cr_count(const tbft_checkpoint_region_t *cr, tbft_seqno_t seqno);

/**
 * Return the winning digest for @p seqno (the one with threshold matches),
 * or NULL if not yet stable.
 */
const tbft_digest_t *tbft_cr_winning_digest(const tbft_checkpoint_region_t *cr,
                                             tbft_seqno_t seqno);

/* --------------------------------------------------------------------------
 * Above-window checkpoint operations
 * -------------------------------------------------------------------------- */

/**
 * Store an above-window Checkpoint from @p replica_id.
 */
void tbft_cr_store_above_window(tbft_checkpoint_region_t *cr,
                                tbft_node_id_t replica_id,
                                const void *msg, int msg_len);

/**
 * Load the above-window Checkpoint for @p replica_id.
 */
const uint8_t *tbft_cr_load_above_window(const tbft_checkpoint_region_t *cr,
                                          tbft_node_id_t replica_id,
                                          int *len_out);

/* --------------------------------------------------------------------------
 * Garbage collection
 * -------------------------------------------------------------------------- */

/**
 * Clear all checkpoint slots with seqno < @p stable_seqno.
 */
void tbft_cr_truncate(tbft_checkpoint_region_t *cr, tbft_seqno_t stable_seqno);
