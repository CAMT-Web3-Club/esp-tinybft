#include "tbft_state.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tbft_state";

/* --------------------------------------------------------------------------
 * CoW bitmap helpers (multi-word for >64 blocks)
 * -------------------------------------------------------------------------- */

static inline void cow_set(tbft_state_t *s, int i)
{
    s->cowb[i / 64] |= (1ULL << (i % 64));
}

static inline bool cow_test(const tbft_state_t *s, int i)
{
    return (s->cowb[i / 64] >> (i % 64)) & 1ULL;
}

static inline void cow_zero(tbft_state_t *s)
{
    memset(s->cowb, 0, sizeof(s->cowb));
}

/* --------------------------------------------------------------------------
 * Fetch-received bitmap helpers (multi-word, mirrors cow_set/test/zero)
 * -------------------------------------------------------------------------- */

static inline void fetch_received_set(tbft_state_t *s, int i)
{
    s->fetch_received[i / 64] |= (1ULL << (i % 64));
}

static inline bool fetch_received_test(const tbft_state_t *s, int i)
{
    return (s->fetch_received[i / 64] >> (i % 64)) & 1ULL;
}

static inline void fetch_received_zero(tbft_state_t *s)
{
    memset(s->fetch_received, 0, sizeof(s->fetch_received));
}

/* --------------------------------------------------------------------------
 * Checkpoint record helpers
 * -------------------------------------------------------------------------- */

static tbft_ckpt_record_t *ckpt_slot_for(tbft_state_t *s, tbft_seqno_t seqno)
{
    int slot = (int)((seqno / TBFT_CHECKPOINT_INTERVAL) % TBFT_NUM_CKPT_SLOTS);
    return &s->ckpt_records[slot];
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

int tbft_state_init(tbft_state_t *state, void *mem, size_t size)
{
    memset(state, 0, sizeof(*state));

    if (!mem || size == 0 || (size % TBFT_BLOCK_SIZE) != 0) {
        ESP_LOGE(TAG, "invalid state memory: ptr=%p size=%zu", mem, size);
        return -1;
    }

    state->mem       = (uint8_t *)mem;
    state->mem_size  = size;
    state->num_blocks = (int)(size / TBFT_BLOCK_SIZE);
    state->fetch_timeout_us = TBFT_RECOVERY_TIMEOUT_US;

    if (state->num_blocks > TBFT_MAX_STATE_BLOCKS) {
        ESP_LOGE(TAG, "too many blocks: %d > %d",
                 state->num_blocks, TBFT_MAX_STATE_BLOCKS);
        return -1;
    }

    /* Initialise partition tree */
    int ret = tbft_ptree_init(&state->ptree, state->num_blocks,
                              TBFT_P_CHILDREN);
    if (ret != 0) {
        return -1;
    }

    /* Pre-allocate each checkpoint record's snapshot array to num_blocks
     * entries.  Every block can change at most once per checkpoint interval,
     * so num_blocks is a hard upper bound.  This avoids realloc() in the
     * CoW hot path and the OOM-induced abort it would otherwise require. */
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        state->ckpt_records[i].old_blocks =
            (tbft_cow_entry_t *)calloc((size_t)state->num_blocks,
                                       sizeof(tbft_cow_entry_t));
        if (!state->ckpt_records[i].old_blocks) {
            ESP_LOGE(TAG, "state init: OOM allocating CoW snapshot array "
                          "(%d blocks * %zu bytes)",
                     state->num_blocks, sizeof(tbft_cow_entry_t));
            for (int j = 0; j < i; j++) {
                free(state->ckpt_records[j].old_blocks);
                state->ckpt_records[j].old_blocks = NULL;
            }
            tbft_ptree_free(&state->ptree);
            return -1;
        }
        state->ckpt_records[i].num_old_blocks = 0;
    }

    ESP_LOGI(TAG, "state init: %d blocks (%zu bytes), CoW reserve "
                  "%d slots * %d blocks * %zu bytes",
             state->num_blocks, size,
             TBFT_NUM_CKPT_SLOTS, state->num_blocks,
             sizeof(tbft_cow_entry_t));
    return 0;
}

void tbft_state_free(tbft_state_t *state)
{
    tbft_ptree_free(&state->ptree);

    /* Free checkpoint record snapshots */
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        if (state->ckpt_records[i].old_blocks) {
            free(state->ckpt_records[i].old_blocks);
            state->ckpt_records[i].old_blocks = NULL;
        }
    }
    memset(state, 0, sizeof(*state));
}

/* --------------------------------------------------------------------------
 * Copy-on-Write
 * -------------------------------------------------------------------------- */

void tbft_state_cow_single(tbft_state_t *state, int bindex)
{
    if (bindex < 0 || bindex >= state->num_blocks) return;
    if (cow_test(state, bindex)) return; /* already snapshotted */

    /* Find the active (most recent) checkpoint record */
    tbft_ckpt_record_t *rec = ckpt_slot_for(state, state->last_stable);

    if (!rec->old_blocks) {
        /* Should not happen: old_blocks is pre-allocated in tbft_state_init
         * and freed only on shutdown.  Log and bail out non-fatally — the
         * missing snapshot merely means we cannot roll back THIS checkpoint
         * on view change; consensus can still advance via state transfer. */
        ESP_LOGW(TAG, "cow_single: snapshot buffer missing, skipping block %d",
                 bindex);
        return;
    }
    if (rec->num_old_blocks >= state->num_blocks) {
        /* Every block already snapshotted — indicates a rapid write pattern
         * on all blocks without an intervening checkpoint.  The CoW bitmap
         * prevents duplicates, so hitting this branch should be impossible. */
        ESP_LOGW(TAG, "cow_single: snapshot array full, skipping block %d",
                 bindex);
        return;
    }

    /* Save the current block into the pre-allocated slot */
    int n = rec->num_old_blocks;
    rec->old_blocks[n].block_idx = bindex;
    memcpy(rec->old_blocks[n].data,
           state->mem + (size_t)bindex * TBFT_BLOCK_SIZE,
           TBFT_BLOCK_SIZE);
    rec->num_old_blocks++;

    cow_set(state, bindex);
}

void tbft_state_cow(tbft_state_t *state, void *mem, size_t size)
{
    /* CRITICAL FIX: Prevent unsigned underflow when size == 0.
     * Without this, end - base - 1 wraps to SIZE_MAX, corrupting the bitmap. */
    if (size == 0) return;

    uintptr_t base  = (uintptr_t)state->mem;
    uintptr_t start = (uintptr_t)mem;
    uintptr_t end   = start + size;

    if (start < base || end > base + state->mem_size) {
        ESP_LOGW(TAG, "cow: range out of state bounds");
        return;
    }

    int first_block = (int)((start - base) / TBFT_BLOCK_SIZE);
    int last_block  = (int)((end - base - 1) / TBFT_BLOCK_SIZE);

    for (int i = first_block; i <= last_block; i++) {
        tbft_state_cow_single(state, i);
    }
}

/* --------------------------------------------------------------------------
 * Checkpointing
 * -------------------------------------------------------------------------- */

void tbft_state_checkpoint(tbft_state_t *state, tbft_seqno_t seqno)
{
    /* Recompute digests for all modified blocks */
    for (int i = 0; i < state->num_blocks; i++) {
        if (!cow_test(state, i)) continue;

        const uint8_t *block = state->mem + (size_t)i * TBFT_BLOCK_SIZE;
        tbft_msg_digest(block, TBFT_BLOCK_SIZE, &state->block_digests[i]);
        if (tbft_ptree_update_leaf(&state->ptree, i, &state->block_digests[i],
                                   seqno) != 0) {
            ESP_LOGW(TAG, "checkpoint: failed to update leaf %d at seqno=%lld",
                     i, (long long)seqno);
            /* Continue — other leaves may succeed */
        }
    }

    /* CRITICAL FIX: Do NOT free old_blocks here. The CoW-saved blocks
     * represent the state at last_stable and are needed for rollback during
     * view-change. Freeing them here makes rollback a silent no-op.
     *
     * Instead, we reset the CoW bitmap and keep the old_blocks intact.
     * The old_blocks will be freed only when mark_stable advances past
     * this checkpoint (in tbft_state_mark_stable). */
    tbft_ckpt_record_t *rec = ckpt_slot_for(state, seqno);
    rec->seqno       = seqno;
    rec->root_digest = *tbft_ptree_root_digest(&state->ptree);
    rec->valid       = true;
    /* Do NOT free old_blocks or reset num_old_blocks here! */

    /* Reset CoW bitmap for next checkpoint interval */
    cow_zero(state);
}

const tbft_digest_t *tbft_state_root_digest(const tbft_state_t *state)
{
    return tbft_ptree_root_digest(&state->ptree);
}

tbft_seqno_t tbft_state_rollback(tbft_state_t *state)
{
    tbft_ckpt_record_t *rec = ckpt_slot_for(state, state->last_stable);
    if (!rec->valid) {
        ESP_LOGW(TAG, "rollback: no valid checkpoint record");
        return 0;
    }

    /* Restore old blocks */
    for (int i = 0; i < rec->num_old_blocks; i++) {
        int bidx = rec->old_blocks[i].block_idx;
        memcpy(state->mem + (size_t)bidx * TBFT_BLOCK_SIZE,
               rec->old_blocks[i].data, TBFT_BLOCK_SIZE);

        /* Rebuild leaf digest */
        tbft_msg_digest(state->mem + (size_t)bidx * TBFT_BLOCK_SIZE,
                        TBFT_BLOCK_SIZE, &state->block_digests[bidx]);
        if (tbft_ptree_update_leaf(&state->ptree, bidx,
                                   &state->block_digests[bidx],
                                   rec->seqno) != 0) {
            ESP_LOGW(TAG, "rollback: failed to rebuild leaf %d at seqno=%lld",
                     bidx, (long long)rec->seqno);
        }
    }

    cow_zero(state);
    return rec->seqno;
}

void tbft_state_mark_stable(tbft_state_t *state, tbft_seqno_t stable_seqno)
{
    if (stable_seqno <= state->last_stable) return;

    /* CRITICAL FIX: Unconditionally reset num_old_blocks for the target
     * checkpoint slot. The slot may have stale CoW data from a previous
     * cycle (when it was used as a CoW target via ckpt_slot_for(last_stable)
     * at a different last_stable value). The valid+seqno guard only catches
     * the case where the slot was a checkpoint target with a different seqno,
     * but misses the case where it was only a CoW target (valid=false). */
    tbft_ckpt_record_t *new_rec = ckpt_slot_for(state, stable_seqno);
    if (new_rec->old_blocks) {
        new_rec->num_old_blocks = 0;
        memset(new_rec->old_blocks, 0,
               (size_t)state->num_blocks * sizeof(tbft_cow_entry_t));
    }

    state->last_stable = stable_seqno;
}

/* --------------------------------------------------------------------------
 * Fetch protocol
 * -------------------------------------------------------------------------- */

void tbft_state_start_fetch(tbft_state_t *state, tbft_seqno_t seqno,
                            int replier)
{
    state->in_fetch         = true;
    state->fetch_seqno      = seqno;
    state->fetch_replier    = replier;
    state->fetch_queue_len  = 0;
    state->n_data_pending   = 0;
    if (state->fetch_timeout_us <= 0) {
        state->fetch_timeout_us = TBFT_RECOVERY_TIMEOUT_US;
    }
    state->fetch_start_time_us = esp_timer_get_time();
    fetch_received_zero(state);

    /* When the partition tree has only one level (all nodes are leaves,
     * which occurs when num_blocks <= p_children), there is no internal-
     * node meta_data to exchange.  Enqueue direct data fetches for every
     * block so state transfer can proceed. */
    if (state->ptree.dims.p_levels == 1) {
        for (int i = 0; i < state->num_blocks; i++) {
            if (state->fetch_queue_len >= TBFT_MAX_STATE_BLOCKS) break;
            state->fetch_queue[state->fetch_queue_len].level = 0;
            state->fetch_queue[state->fetch_queue_len].index = i;
            state->fetch_queue[state->fetch_queue_len].done  = false;
            state->fetch_queue_len++;
            /* Do NOT increment n_data_pending here — next_fetch_req does it
             * when it sees level 0 == p_levels - 1 (the leaf level). */
        }
    } else {
        /* Enqueue a root-level meta_data fetch request */
        state->fetch_queue[0].level = 0;
        state->fetch_queue[0].index = 0;
        state->fetch_queue[0].done  = false;
        state->fetch_queue_len      = 1;
    }

    ESP_LOGI(TAG, "start_fetch seqno=%lld replier=%d",
             (long long)seqno, replier);
}

void tbft_state_handle_meta_data(tbft_state_t *state,
                                 const tbft_meta_data_rep_t *rep,
                                 const tbft_part_info_t *parts,
                                 int n_parts)
{
    if (!state->in_fetch) return;
    if (rep->seqno != state->fetch_seqno) return;

    int level = rep->level;
    int index = rep->index;
    int pchildren = state->ptree.dims.p_children;
    int p_levels  = state->ptree.dims.p_levels;

    /* Validate inputs before using them to index into state->ptree.ptree[].
     * A malicious Meta_data with out-of-range level/index/n_parts would
     * otherwise OOB-index the partition tree array. */
    if (level < 0 || level >= p_levels - 1) {
        ESP_LOGW(TAG, "meta_data: invalid level %d (p_levels=%d)",
                 level, p_levels);
        return;
    }
    int nodes_at_level = tbft_ptree_nodes_at_level(level, pchildren);
    if (index < 0 || index >= nodes_at_level) {
        ESP_LOGW(TAG, "meta_data: invalid index %d at level %d (max=%d)",
                 index, level, nodes_at_level);
        return;
    }
    if (n_parts < 0 || n_parts > pchildren) {
        ESP_LOGW(TAG, "meta_data: invalid n_parts %d (pchildren=%d)",
                 n_parts, pchildren);
        return;
    }

    int child_level = level + 1;
    int child_nodes_at_level =
        tbft_ptree_nodes_at_level(child_level, pchildren);
    /* NOTE: is_leaf_children was previously computed here but never used.
     * tbft_state_next_fetch_req handles the leaf vs internal differentiation
     * by checking if level == p_levels - 1. */

    for (int i = 0; i < n_parts; i++) {
        int child_idx = index * pchildren + i;
        if (child_idx < 0 || child_idx >= child_nodes_at_level) {
            ESP_LOGW(TAG, "meta_data: child_idx %d OOB at level %d",
                     child_idx, child_level);
            continue;
        }

        /* Both internal and leaf children use the same enqueue logic —
         * tbft_state_next_fetch_req differentiates by checking whether the
         * level is the leaf level (to bump n_data_pending). */
        const tbft_digest_t *local =
            &state->ptree.ptree[child_level][child_idx].digest;
        if (!tbft_digest_equal(local, &parts[i].digest)) {
            if (state->fetch_queue_len < TBFT_MAX_STATE_BLOCKS) {
                tbft_fetch_req_t *req =
                    &state->fetch_queue[state->fetch_queue_len++];
                req->level = child_level;
                req->index = child_idx;
                req->done  = false;
            } else {
                ESP_LOGW(TAG, "meta_data: fetch queue full, dropping "
                         "child level=%d idx=%d (max=%d)",
                         child_level, child_idx, TBFT_MAX_STATE_BLOCKS);
            }
        }
    }
}

void tbft_state_handle_data(tbft_state_t *state,
                            const tbft_data_rep_t *rep,
                            const uint8_t *block_data)
{
    if (!state->in_fetch) return;
    if (rep->seqno != state->fetch_seqno) return;

    int bidx = rep->block_index;
    if (bidx < 0 || bidx >= state->num_blocks) return;

    /* Deduplicate: ignore blocks already received (e.g. network retransmits).
     * Without this, a duplicate data message would decrement n_data_pending
     * a second time, potentially triggering fetch_complete prematurely before
     * all blocks have arrived. */
    if (fetch_received_test(state, bidx)) return;

    /* Verify block digest */
    tbft_digest_t computed;
    tbft_msg_digest(block_data, TBFT_BLOCK_SIZE, &computed);
    if (!tbft_digest_equal(&computed, &rep->digest)) {
        ESP_LOGW(TAG, "handle_data: block %d digest mismatch", bidx);
        return;
    }

    /* Write block to state */
    memcpy(state->mem + (size_t)bidx * TBFT_BLOCK_SIZE,
           block_data, TBFT_BLOCK_SIZE);
    state->block_digests[bidx] = computed;
    if (tbft_ptree_update_leaf(&state->ptree, bidx, &computed, rep->seqno) != 0) {
        ESP_LOGW(TAG, "handle_data: failed to update leaf %d", bidx);
        /* Block was written — continue with best-effort tree state */
    }

    /* Mark received so retransmits don't double-decrement */
    fetch_received_set(state, bidx);

    /* HIGH FIX H6: Decrement pending count safely (don't go below 0) */
    if (state->n_data_pending > 0) {
        state->n_data_pending--;
    }

    /* Check if all pending fetches are resolved */
    bool all_done = (state->n_data_pending == 0);
    if (all_done) {
        for (int i = 0; i < state->fetch_queue_len; i++) {
            if (!state->fetch_queue[i].done) {
                all_done = false;
                break;
            }
        }
    }
    if (all_done) {
        tbft_state_fetch_complete(state);
    }
}

bool tbft_state_next_fetch_req(tbft_state_t *state, int *level, int *index)
{
    for (int i = 0; i < state->fetch_queue_len; i++) {
        if (!state->fetch_queue[i].done) {
            *level = state->fetch_queue[i].level;
            *index = state->fetch_queue[i].index;
            state->fetch_queue[i].done = true;
            if (*level == state->ptree.dims.p_levels - 1) {
                /* A data message may have arrived before this request was
                 * dequeued (network reordering or unsolicited replier).  If
                 * so, handle_data already wrote and marked the block — do
                 * NOT increment n_data_pending or the fetch will hang. */
                if (!fetch_received_test(state, *index)) {
                    state->n_data_pending++;
                }
            }
            return true;
        }
    }
    return false;
}

void tbft_state_fetch_complete(tbft_state_t *state)
{
    ESP_LOGI(TAG, "fetch complete seqno=%lld", (long long)state->fetch_seqno);
    state->in_fetch        = false;
    state->fetch_queue_len = 0;
    state->n_data_pending  = 0;
    fetch_received_zero(state);
}

bool tbft_state_get_checkpoint_digest(const tbft_state_t *state,
                                      tbft_seqno_t seqno,
                                      tbft_digest_t *digest_out)
{
    int slot = (int)((seqno / TBFT_CHECKPOINT_INTERVAL) % TBFT_NUM_CKPT_SLOTS);
    const tbft_ckpt_record_t *rec = &state->ckpt_records[slot];
    if (rec->valid && rec->seqno == seqno) {
        if (digest_out) {
            *digest_out = rec->root_digest;
        }
        return true;
    }
    return false;
}
