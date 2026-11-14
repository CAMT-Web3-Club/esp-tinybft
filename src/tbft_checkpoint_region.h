#pragma once

#include "tbft_config.h"
#include "tbft_types.h"
#include "tbft_message.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * checkpoint_region — static storage for Checkpoint messages (section 7).
 *
 * Checkpoint slots indexed by (seqno/interval) % num_slots; each holds one
 * Checkpoint message per replica (MAX_NUM_REPLICAS) for the seqno it is
 * tagged with.  A checkpoint far ahead of this replica takes over the slot it
 * maps to, so a replica that fell behind still collects the certificate it
 * needs to fetch the state.
 * -------------------------------------------------------------------------- */

/* Raw bytes for one Checkpoint message on the wire */
static constexpr size_t TBFT_CKPT_MSG_SIZE = sizeof(tbft_checkpoint_rep_t) + TBFT_AUTH_SIZE;

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
 * @return message bytes or nullptr
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
 * or nullptr if not yet stable.
 */
const tbft_digest_t *tbft_cr_winning_digest(const tbft_checkpoint_region_t *cr,
                                             tbft_seqno_t seqno);

/* --------------------------------------------------------------------------
 * Garbage collection
 * -------------------------------------------------------------------------- */

/**
 * Clear all checkpoint slots with seqno < @p stable_seqno.
 */
void tbft_cr_truncate(tbft_checkpoint_region_t *cr, tbft_seqno_t stable_seqno);
