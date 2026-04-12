#include "tbft_checkpoint_region.h"
#include <string.h>

static inline int slot_index(tbft_seqno_t seqno)
{
    return (int)((seqno / TBFT_CHECKPOINT_INTERVAL) % TBFT_NUM_CKPT_SLOTS);
}

void tbft_cr_init(tbft_checkpoint_region_t *cr,
                  int num_replicas, int threshold)
{
    memset(cr, 0, sizeof(*cr));
    cr->num_replicas = num_replicas;
    cr->threshold    = threshold;
}

bool tbft_cr_store(tbft_checkpoint_region_t *cr,
                   tbft_seqno_t seqno,
                   tbft_node_id_t replica_id,
                   const void *msg, int msg_len)
{
    if (replica_id < 0 || replica_id >= TBFT_MAX_NUM_REPLICAS) return false;
    if (msg_len > (int)TBFT_CKPT_MSG_SIZE) return false;

    int sidx = slot_index(seqno);
    tbft_ckpt_slot_t *slot = &cr->slots[sidx];

    if (slot->present[replica_id]) return false; /* duplicate */

    memcpy(slot->msgs[replica_id], msg, (size_t)msg_len);
    slot->msg_lens[replica_id] = msg_len;
    slot->present[replica_id]  = true;

    /* Extract digest from message and check for matches */
    const tbft_checkpoint_rep_t *rep = (const tbft_checkpoint_rep_t *)msg;

    if (slot->match_count == 0) {
        /* First message — set as candidate winning digest */
        slot->winning_digest = rep->digest;
        slot->match_count    = 1;
    } else if (tbft_digest_equal(&slot->winning_digest, &rep->digest)) {
        slot->match_count++;
    }

    return slot->match_count >= cr->threshold;
}

const uint8_t *tbft_cr_load(const tbft_checkpoint_region_t *cr,
                             tbft_seqno_t seqno,
                             tbft_node_id_t replica_id, int *len_out)
{
    if (replica_id < 0 || replica_id >= TBFT_MAX_NUM_REPLICAS) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    int sidx = slot_index(seqno);
    const tbft_ckpt_slot_t *slot = &cr->slots[sidx];
    if (!slot->present[replica_id]) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    if (len_out) *len_out = slot->msg_lens[replica_id];
    return slot->msgs[replica_id];
}

int tbft_cr_count(const tbft_checkpoint_region_t *cr, tbft_seqno_t seqno)
{
    return cr->slots[slot_index(seqno)].match_count;
}

const tbft_digest_t *tbft_cr_winning_digest(const tbft_checkpoint_region_t *cr,
                                             tbft_seqno_t seqno)
{
    const tbft_ckpt_slot_t *slot = &cr->slots[slot_index(seqno)];
    if (slot->match_count < cr->threshold) return NULL;
    return &slot->winning_digest;
}

void tbft_cr_store_above_window(tbft_checkpoint_region_t *cr,
                                tbft_node_id_t replica_id,
                                const void *msg, int msg_len)
{
    if (replica_id < 0 || replica_id >= TBFT_MAX_NUM_REPLICAS) return;
    if (msg_len > (int)TBFT_CKPT_MSG_SIZE) return;

    tbft_ckpt_slot_t *slot = &cr->above_window[replica_id];
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->msgs[0], msg, (size_t)msg_len);
    slot->msg_lens[0]  = msg_len;
    slot->present[0]   = true;
    slot->match_count  = 1;
}

const uint8_t *tbft_cr_load_above_window(const tbft_checkpoint_region_t *cr,
                                          tbft_node_id_t replica_id,
                                          int *len_out)
{
    if (replica_id < 0 || replica_id >= TBFT_MAX_NUM_REPLICAS) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    const tbft_ckpt_slot_t *slot = &cr->above_window[replica_id];
    if (!slot->present[0]) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    if (len_out) *len_out = slot->msg_lens[0];
    return slot->msgs[0];
}

void tbft_cr_truncate(tbft_checkpoint_region_t *cr, tbft_seqno_t stable_seqno)
{
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        /* Reconstruct the seqno for this slot (heuristic: check first present) */
        tbft_ckpt_slot_t *slot = &cr->slots[i];
        for (int r = 0; r < TBFT_MAX_NUM_REPLICAS; r++) {
            if (slot->present[r] && slot->msg_lens[r] >= (int)sizeof(tbft_checkpoint_rep_t)) {
                const tbft_checkpoint_rep_t *rep =
                    (const tbft_checkpoint_rep_t *)slot->msgs[r];
                if (rep->seqno < stable_seqno) {
                    memset(slot, 0, sizeof(*slot));
                }
                break;
            }
        }
    }
}
