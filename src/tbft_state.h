#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_partition.h"
#include "esp_timer.h"

/* --------------------------------------------------------------------------
 * State — manages the replica's application state (section 5 / State).
 *
 *  - Flat array of fixed-size Blocks (TBFT_BLOCK_SIZE each)
 *  - Copy-on-Write (CoW) bitmap: before any block write, cow_single saves it
 *  - Partition tree (ptree + stree) for incremental Merkle digest
 *  - Checkpoint log: CheckpointRecord per checkpoint interval
 *  - Fetch protocol: receives Meta_data / Data messages to recover state
 *
 * Note: TBFT_MAX_STATE_BLOCKS, TBFT_NUM_CKPT_SLOTS are defined in tbft_config.h
 * -------------------------------------------------------------------------- */

/* One checkpoint record: the state digest at a checkpoint seqno */
typedef struct {
    tbft_seqno_t    seqno;
    tbft_digest_t   root_digest;      /* state digest at this seqno */
    bool            valid;
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
    size_t        mem_size;      /* application state bytes */
    int           app_blocks;    /* mem_size / TBFT_BLOCK_SIZE */
    /* Library state (client table), checkpointed and transferred exactly
     * like application blocks; it follows them in block numbering. */
    uint8_t      *sys_mem;
    int           num_blocks;    /* app_blocks + system blocks */

    /* Dirty bitmap (bit i = block i modified since the last checkpoint) */
    tbft_bitmap_t cowb[TBFT_MAX_STATE_BLOCKS / 64 + 1];

    /* Partition tree */
    tbft_ptree_t  ptree;

    /* Per-block current digests (leaf level of ptree) */
    tbft_digest_t block_digests[TBFT_MAX_STATE_BLOCKS];

    /* Checkpoint records */
    tbft_ckpt_record_t ckpt_records[TBFT_NUM_CKPT_SLOTS];
    int                ckpt_head;   /* index of oldest valid record */
    int                ckpt_count;

    /* Fetch state */
    bool              in_fetch;
    tbft_seqno_t      fetch_seqno;
    tbft_fetch_req_t  fetch_queue[TBFT_MAX_STATE_BLOCKS];
    int               fetch_queue_len;
    int               n_data_pending; /* number of leaf requests dispatched but not fulfilled */
    int64_t           fetch_timeout_us;
    int64_t           fetch_start_time_us;
    int               fetch_replier;  /* replica id we're fetching from */
    tbft_bitmap_t     fetch_received[(TBFT_MAX_STATE_BLOCKS + 63) / 64]; /* blocks already received */

    /* Target of the fetch: the root digest the fetched state must have */
    bool              fetch_has_digest;
    tbft_digest_t     fetch_digest;

    /* Copy of every block at the latest checkpoint (image_seqno); served to
     * replicas that fetch that checkpoint, so they get exactly the state the
     * checkpoint digest covers even while this replica executes further. */
    uint8_t          *image;
    tbft_seqno_t      image_seqno;

    /* Stable seqno (updated when checkpoints become stable) */
    tbft_seqno_t      last_stable;
} tbft_state_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise state with the application's memory region plus the library's
 * own replicated region (blocks app_blocks .. num_blocks-1).
 * @param state     State to initialise
 * @param mem       Application memory buffer
 * @param size      Buffer size (must be a multiple of TBFT_BLOCK_SIZE)
 * @param sys_mem   Library memory buffer (may be nullptr if sys_size is 0)
 * @param sys_size  Its size (a multiple of TBFT_BLOCK_SIZE)
 * @return 0 on success, -1 on error
 */
int tbft_state_init(tbft_state_t *state, void *mem, size_t size,
                    void *sys_mem, size_t sys_size);

/** Address of block @p i, application or library region. */
static inline uint8_t *tbft_state_block(const tbft_state_t *state, int i)
{
    return i < state->app_blocks
        ? state->mem + (size_t)i * TBFT_BLOCK_SIZE
        : state->sys_mem + (size_t)(i - state->app_blocks) * TBFT_BLOCK_SIZE;
}

/**
 * Release resources held by state.
 */
void tbft_state_free(tbft_state_t *state);

/* --------------------------------------------------------------------------
 * Copy-on-Write
 * -------------------------------------------------------------------------- */

/**
 * Mark block @p bindex as modified (call before or after changing it); its
 * digest is recomputed at the next checkpoint.
 */
void tbft_state_cow_single(tbft_state_t *state, int bindex);

/**
 * Mark all application blocks in [mem, mem+size) as modified.
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
 *  - Store CheckpointRecord and copy the state into the served image
 *  - Reset the dirty bitmap
 */
void tbft_state_checkpoint(tbft_state_t *state, tbft_seqno_t seqno);

/**
 * Return the root state digest at the most recent checkpoint.
 */
const tbft_digest_t *tbft_state_root_digest(const tbft_state_t *state);


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
                            int replier, const tbft_digest_t *target_digest);

/** After a completed fetch: adopt the fetched state as checkpoint @p seqno. */
void tbft_state_install_fetched(tbft_state_t *state, tbft_seqno_t seqno);

/** True when this replica can serve checkpoint @p seqno to a fetcher. */
static inline bool tbft_state_can_serve(const tbft_state_t *state,
                                        tbft_seqno_t seqno)
{
    return !state->in_fetch && state->image && state->image_seqno == seqno;
}

/** Block @p i of the image served for checkpoint image_seqno. */
static inline const uint8_t *tbft_state_image_block(const tbft_state_t *state,
                                                    int i)
{
    return state->image + (size_t)i * TBFT_BLOCK_SIZE;
}

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
