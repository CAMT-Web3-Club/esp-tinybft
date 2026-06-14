#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_partition.h"
#include "esp_timer.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * State — manages the replica's application state (section 5 / State).
 *
 *  - Flat array of fixed-size Blocks (TBFT_BLOCK_SIZE each)
 *  - Copy-on-Write (CoW) bitmap: before any block write, cow_single saves it
 *  - Partition tree (ptree) for incremental Merkle digest
 *  - Checkpoint log: CheckpointRecord per checkpoint interval
 *  - Fetch protocol: receives Meta_data / Data messages to recover state
 *
 * Note: TBFT_MAX_STATE_BLOCKS, TBFT_NUM_CKPT_SLOTS are defined in tbft_config.h
 * -------------------------------------------------------------------------- */

/* One snapshot of a block saved before modification (for CoW) */
typedef struct {
    int       block_idx;
    uint8_t   data[TBFT_BLOCK_SIZE];
} tbft_cow_entry_t;

/* One checkpoint record (snapshot at a stable seqno) */
typedef struct {
    tbft_seqno_t    seqno;
    tbft_digest_t   root_digest;      /* state digest at this seqno */
    tbft_cow_entry_t *old_blocks;     /* heap array of block snapshots */
    int              num_old_blocks;
    bool             valid;
} tbft_ckpt_record_t;

/* State fetch request entry */
typedef struct {
    int  level;
    int  index;
    bool done;
} tbft_fetch_req_t;

typedef struct {
    /* Application state */
    uint8_t      *mem;           /* pointer to application-managed memory */
    size_t        mem_size;      /* total state bytes */
    int           num_blocks;    /* mem_size / TBFT_BLOCK_SIZE */

    /* Copy-on-Write bitmap (bit i = block i has been snapshotted) */
    tbft_bitmap_t cowb[TBFT_MAX_STATE_BLOCKS / 64 + 1];

    /* Partition tree */
    tbft_ptree_t  ptree;

    /* Per-block current digests (leaf level of ptree) */
    tbft_digest_t block_digests[TBFT_MAX_STATE_BLOCKS];

    /* Checkpoint records */
    tbft_ckpt_record_t ckpt_records[TBFT_NUM_CKPT_SLOTS];
    /* A5-F008 FIX: removed dead state ckpt_head/ckpt_count (never read/written) */

    /* Fetch state */
    bool              in_fetch;
    tbft_seqno_t      fetch_seqno;
    tbft_fetch_req_t  fetch_queue[TBFT_MAX_STATE_BLOCKS * 2];
    int               fetch_queue_len;
    int               n_data_pending; /* number of leaf requests dispatched but not fulfilled */
    int64_t           fetch_timeout_us;
    int64_t           fetch_start_time_us;
    int               fetch_replier;  /* replica id we're fetching from */
    tbft_bitmap_t     fetch_received[(TBFT_MAX_STATE_BLOCKS + 63) / 64]; /* blocks already received */

    /* Stable seqno (updated when checkpoints become stable) */
    tbft_seqno_t      last_stable;

    /* CoW target: the seqno that CoW and rollback should use.
     * Decoupled from last_stable so that mid-interval mark_stable
     * (from state fetch) does not misalign the CoW snapshot with
     * the rollback target.  Advances only in tbft_state_checkpoint. */
    tbft_seqno_t      cow_target_seqno;
} tbft_state_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise state with the application's memory region.
 * @param state   State to initialise
 * @param mem     Application memory buffer
 * @param size    Buffer size (must be a multiple of TBFT_BLOCK_SIZE)
 * @return 0 on success, -1 on error
 */
int tbft_state_init(tbft_state_t *state, void *mem, size_t size);

/**
 * Release resources held by state.
 */
void tbft_state_free(tbft_state_t *state);

/* --------------------------------------------------------------------------
 * Copy-on-Write
 * -------------------------------------------------------------------------- */

/**
 * Mark block @p bindex for Copy-on-Write before the application modifies it.
 * Saves the current block to the most recent checkpoint's old_blocks list.
 */
void tbft_state_cow_single(tbft_state_t *state, int bindex);

/**
 * Mark all blocks in [mem, mem+size) for CoW.
 * @param mem  Pointer within the state buffer
 * @param size Number of bytes to cover
 */
void tbft_state_cow(tbft_state_t *state, void *mem, size_t size);

/* --------------------------------------------------------------------------
 * Checkpointing
 * -------------------------------------------------------------------------- */

/**
 * Take a checkpoint at @p seqno:
 *  - Compute SHA-256 for all modified blocks (where cowb[i] = 1)
 *  - Update partition tree
 *  - Store CheckpointRecord
 *  - Reset CoW bitmap
 */
void tbft_state_checkpoint(tbft_state_t *state, tbft_seqno_t seqno);

/**
 * Return the root state digest at the most recent checkpoint.
 */
const tbft_digest_t *tbft_state_root_digest(const tbft_state_t *state);

/**
 * Rollback state to the last stable checkpoint.
 * @return seqno of the stable checkpoint restored
 */
tbft_seqno_t tbft_state_rollback(tbft_state_t *state);

/**
 * Mark a checkpoint as stable across the network.
 * Discards CoW history for checkpoints prior to this seqno.
 */
void tbft_state_mark_stable(tbft_state_t *state, tbft_seqno_t stable_seqno);

/* --------------------------------------------------------------------------
 * State transfer (fetch protocol)
 * -------------------------------------------------------------------------- */

/**
 * Begin fetching state for checkpoint @p seqno from replica @p replier.
 */
void tbft_state_start_fetch(tbft_state_t *state, tbft_seqno_t seqno,
                            int replier);

/**
 * Handle an incoming Meta_data message during fetch.
 * Enqueues sub-tree fetches for mismatched children.
 */
void tbft_state_handle_meta_data(tbft_state_t *state,
                                 const tbft_meta_data_rep_t *rep,
                                 const tbft_part_info_t *parts,
                                 int n_parts);

/**
 * Handle an incoming Data message during fetch.
 * Verifies the block digest and writes the block.
 */
void tbft_state_handle_data(tbft_state_t *state,
                            const tbft_data_rep_t *rep,
                            const uint8_t *block_data);

/**
 * Returns true if a state fetch is in progress.
 */
static inline bool tbft_state_in_fetch(const tbft_state_t *state)
{
    return state->in_fetch;
}

/**
 * Check if the current fetch has timed out.
 * @return true if fetch has exceeded timeout
 */
static inline bool tbft_state_fetch_timedout(const tbft_state_t *state)
{
    return state->in_fetch &&
           (esp_timer_get_time() - state->fetch_start_time_us) > state->fetch_timeout_us;
}

/**
 * Dequeue the next fetch request to send (level/index pair).
 * @return true if a request was dequeued
 */
bool tbft_state_next_fetch_req(tbft_state_t *state,
                               int *level, int *index);

/**
 * Called when all missing blocks have been received.
 * Signals completion and exits fetch mode.
 */
void tbft_state_fetch_complete(tbft_state_t *state);

/**
 * Retrieve the root state digest for a past checkpoint sequence number.
 * @param state       State to query
 * @param seqno       Sequence number of the checkpoint
 * @param digest_out  Pointer to write the digest to (optional)
 * @return true if a valid checkpoint record for this seqno was found, false otherwise
 */
bool tbft_state_get_checkpoint_digest(const tbft_state_t *state,
                                      tbft_seqno_t seqno,
                                      tbft_digest_t *digest_out);
