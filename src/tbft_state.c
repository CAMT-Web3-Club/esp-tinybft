#include "tbft_state.h"
#include "esp_log.h"
#include <string.h>

static constexpr char TAG[] = "tbft_state";

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

int tbft_state_init(tbft_state_t *state, void *mem, size_t size,
                    void *sys_mem, size_t sys_size)
{
    memset(state, 0, sizeof(*state));

    if (!mem || size == 0 || (size % TBFT_BLOCK_SIZE) != 0) {
        ESP_LOGE(TAG, "invalid state memory: ptr=%p size=%zu", mem, size);
        return -1;
    }
    if ((sys_size % TBFT_BLOCK_SIZE) != 0 || (sys_size > 0 && !sys_mem)) {
        ESP_LOGE(TAG, "invalid library state memory: ptr=%p size=%zu", sys_mem, sys_size);
        return -1;
    }

    state->mem        = (uint8_t *)mem;
    state->mem_size   = size;
    state->app_blocks = (int)(size / TBFT_BLOCK_SIZE);
    state->sys_mem    = (uint8_t *)sys_mem;
    state->num_blocks = state->app_blocks + (int)(sys_size / TBFT_BLOCK_SIZE);
    state->fetch_timeout_us = TBFT_RECOVERY_TIMEOUT_US;

    if (state->num_blocks > TBFT_MAX_STATE_BLOCKS) {
        ESP_LOGE(TAG, "too many blocks: %d application + %d library > "
                 "TBFT_MAX_STATE_BLOCKS %d", state->app_blocks,
                 state->num_blocks - state->app_blocks, TBFT_MAX_STATE_BLOCKS);
        return -1;
    }

    /* Initialise partition tree */
    int ret = tbft_ptree_init(&state->ptree, state->num_blocks,
                              TBFT_P_CHILDREN);
    if (ret != 0) {
        return -1;
    }

    /* Checkpoint 0: the initial state every replica starts from. */
    for (int i = 0; i < state->num_blocks; i++) {
        tbft_state_cow_single(state, i);
    }
    tbft_state_checkpoint(state, 0);

    ESP_LOGI(TAG, "state init: %d blocks (%d application + %d library, %zu bytes each)",
             state->num_blocks, state->app_blocks,
             state->num_blocks - state->app_blocks, (size_t)TBFT_BLOCK_SIZE);
    return 0;
}

void tbft_state_free(tbft_state_t *state)
{
    tbft_ptree_free(&state->ptree);
    memset(state, 0, sizeof(*state));
}

/* --------------------------------------------------------------------------
 * Copy-on-Write
 * -------------------------------------------------------------------------- */

void tbft_state_cow_single(tbft_state_t *state, int bindex)
{
    if (bindex < 0 || bindex >= state->num_blocks) return;
    cow_set(state, bindex);   /* digest recomputed at the next checkpoint */
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

        const uint8_t *block = tbft_state_block(state, i);
        tbft_msg_digest(block, TBFT_BLOCK_SIZE, &state->block_digests[i]);
        if (tbft_ptree_update_leaf(&state->ptree, i, &state->block_digests[i],
                                   seqno) != 0) {
            ESP_LOGW(TAG, "checkpoint: failed to update leaf %d at seqno=%lld",
                     i, (long long)seqno);
            /* Continue — other leaves may succeed */
        }
    }

    tbft_ckpt_record_t *rec = ckpt_slot_for(state, seqno);
    rec->seqno       = seqno;
    rec->root_digest = *tbft_ptree_root_digest(&state->ptree);
    rec->valid       = true;

    for (int i = 0; i < state->num_blocks; i++) {
        memcpy(state->image + (size_t)i * TBFT_BLOCK_SIZE,
               tbft_state_block(state, i), TBFT_BLOCK_SIZE);
    }
    state->image_seqno = seqno;

    /* Reset CoW bitmap for next checkpoint interval */
    cow_zero(state);
}

const tbft_digest_t *tbft_state_root_digest(const tbft_state_t *state)
{
    return tbft_ptree_root_digest(&state->ptree);
}

void tbft_state_mark_stable(tbft_state_t *state, tbft_seqno_t stable_seqno)
{
    if (stable_seqno <= state->last_stable) return;

    state->last_stable = stable_seqno;
}

/* --------------------------------------------------------------------------
 * Fetch protocol
 * -------------------------------------------------------------------------- */

/* Bring the digests of blocks modified since the last checkpoint up to date,
 * so a fetch compares the CURRENT content with the replier's checkpoint and
 * also replaces blocks this replica changed after its own checkpoint. */
static void refresh_dirty_digests(tbft_state_t *state)
{
    for (int i = 0; i < state->num_blocks; i++) {
        if (!cow_test(state, i)) continue;
        tbft_msg_digest(tbft_state_block(state, i), TBFT_BLOCK_SIZE,
                        &state->block_digests[i]);
        tbft_ptree_update_leaf(&state->ptree, i, &state->block_digests[i],
                               state->image_seqno);
    }
}

void tbft_state_start_fetch(tbft_state_t *state, tbft_seqno_t seqno,
                            int replier, const tbft_digest_t *target_digest)
{
    refresh_dirty_digests(state);
    state->fetch_has_digest = target_digest != nullptr;
    if (target_digest) {
        state->fetch_digest = *target_digest;
    } else {
        tbft_digest_zero(&state->fetch_digest);
    }
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
    memcpy(tbft_state_block(state, bidx),
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

void tbft_state_install_fetched(tbft_state_t *state, tbft_seqno_t seqno)
{
    /* Every block now holds the fetched checkpoint: recompute all digests
     * and make it this replica's latest checkpoint (and served image). */
    for (int i = 0; i < state->num_blocks; i++) {
        cow_set(state, i);
    }
    tbft_state_checkpoint(state, seqno);
    if (seqno > state->last_stable) state->last_stable = seqno;
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
