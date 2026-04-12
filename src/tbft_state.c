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
 * Checkpoint record helpers
 * -------------------------------------------------------------------------- */

static tbft_ckpt_record_t *ckpt_slot_for(tbft_state_t *s, tbft_seqno_t seqno)
{
    int slot = (int)((seqno / TBFT_CHECKPOINT_INTERVAL) % TBFT_MAX_CKPT_RECORDS);
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

    ESP_LOGI(TAG, "state init: %d blocks (%zu bytes)", state->num_blocks, size);
    return 0;
}

void tbft_state_free(tbft_state_t *state)
{
    tbft_ptree_free(&state->ptree);

    /* Free checkpoint record snapshots */
    for (int i = 0; i < TBFT_MAX_CKPT_RECORDS; i++) {
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

    /* Grow old_blocks array */
    int n = rec->num_old_blocks;
    tbft_cow_entry_t *new_arr = (tbft_cow_entry_t *)realloc(
        rec->old_blocks, (size_t)(n + 1) * sizeof(tbft_cow_entry_t));
    if (!new_arr) {
        ESP_LOGE(TAG, "cow_single: out of memory");
        return;
    }
    rec->old_blocks = new_arr;

    /* Save the current block */
    rec->old_blocks[n].block_idx = bindex;
    memcpy(rec->old_blocks[n].data,
           state->mem + (size_t)bindex * TBFT_BLOCK_SIZE,
           TBFT_BLOCK_SIZE);
    rec->num_old_blocks++;

    cow_set(state, bindex);
}

void tbft_state_cow(tbft_state_t *state, void *mem, size_t size)
{
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
        tbft_ptree_update_leaf(&state->ptree, i, &state->block_digests[i],
                               (int32_t)seqno);
    }

    /* Store the checkpoint record */
    tbft_ckpt_record_t *rec = ckpt_slot_for(state, seqno);
    rec->seqno       = seqno;
    rec->root_digest = *tbft_ptree_root_digest(&state->ptree);
    rec->valid       = true;

    /* Reset CoW bitmap */
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
        tbft_ptree_update_leaf(&state->ptree, bidx,
                               &state->block_digests[bidx],
                               (int32_t)rec->seqno);
    }

    cow_zero(state);
    return rec->seqno;
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
    state->fetch_timeout_us = 100000; /* 100 ms */

    /* Enqueue a root-level fetch request */
    state->fetch_queue[0].level = 0;
    state->fetch_queue[0].index = 0;
    state->fetch_queue[0].done  = false;
    state->fetch_queue_len      = 1;

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
    int pchildren = state->ptree.dims.p_children;

    for (int i = 0; i < n_parts; i++) {
        int child_idx = rep->index * pchildren + i;
        int child_level = level + 1;

        /* Check if local digest matches */
        if (child_level < state->ptree.dims.p_levels) {
            const tbft_digest_t *local =
                &state->ptree.ptree[child_level][child_idx].digest;
            if (!tbft_digest_equal(local, &parts[i].digest)) {
                /* Mismatch: enqueue fetch for this child */
                if (state->fetch_queue_len < TBFT_MAX_STATE_BLOCKS) {
                    tbft_fetch_req_t *req =
                        &state->fetch_queue[state->fetch_queue_len++];
                    req->level = child_level;
                    req->index = child_idx;
                    req->done  = false;
                }
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
    tbft_ptree_update_leaf(&state->ptree, bidx, &computed,
                           (int32_t)rep->seqno);

    /* Check if all pending fetches are resolved */
    bool all_done = true;
    for (int i = 0; i < state->fetch_queue_len; i++) {
        if (!state->fetch_queue[i].done) {
            all_done = false;
            break;
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
}
