#include "tbft_checkpoint_region.h"
#include <string.h>

static inline int slot_index(tbft_seqno_t seqno)
{
    /* tbft_seqno_t is int64_t; negative seqno would indicate a logic bug
     * in the caller. The division-by-zero guard in Kconfig (static_assert
     * TBFT_CHECKPOINT_INTERVAL >= 1) ensures the modulo is safe. */
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
    if (msg_len < (int)sizeof(tbft_checkpoint_rep_t) ||
        msg_len > (int)TBFT_CKPT_MSG_SIZE) return false;

    int sidx = slot_index(seqno);
    tbft_ckpt_slot_t *slot = &cr->slots[sidx];

    /* Slot aliasing: slot_index wraps modulo TBFT_NUM_CKPT_SLOTS, so two
     * distinct checkpoint seqnos map to the same slot once the caller has
     * advanced past one full window.  If the slot's stored seqno does not
     * match, the old seqno's votes/digests/candidates are stale and must
     * be discarded before we record the new ones — otherwise stale-vote
     * poisoning can spuriously complete a quorum. */
    if (slot->seqno != seqno) {
        memset(slot, 0, sizeof(*slot));
        slot->seqno = seqno;
    }

    if (slot->present[replica_id]) return false; /* duplicate */

    memcpy(slot->msgs[replica_id], msg, (size_t)msg_len);
    slot->msg_lens[replica_id] = msg_len;
    slot->present[replica_id]  = true;

    /* Multi-candidate digest tracking.
     * Find or register this message's digest among the tracked candidates.
     * This prevents a Byzantine replica that arrives first from permanently
     * blocking a legitimate quorum by establishing a wrong winning_digest. */
    const tbft_checkpoint_rep_t *rep = (const tbft_checkpoint_rep_t *)msg;

    int ci = -1;
    for (int i = 0; i < slot->n_candidates; i++) {
        if (tbft_digest_equal(&slot->cand_digests[i], &rep->digest)) {
            ci = i;
            break;
        }
    }
    if (ci < 0) {
        if (slot->n_candidates < TBFT_CERT_MAX_VALS) {
            ci = slot->n_candidates++;
            slot->cand_digests[ci] = rep->digest;
            slot->cand_counts[ci]  = 0;
        } else {
            /* All candidate slots taken by distinct values: skip this one.
             * With ≥ TBFT_CERT_MAX_VALS distinct digests, no single value
             * can reach 2f+1 (pigeonhole), so the slot will never become
             * stable regardless. */
            return false;
        }
    }
    slot->cand_counts[ci]++;

    /* Keep winning_digest pointing to the candidate with the most votes */
    if (slot->cand_counts[ci] > slot->match_count) {
        slot->match_count    = slot->cand_counts[ci];
        slot->winning_digest = slot->cand_digests[ci];
    }

    return slot->match_count >= cr->threshold;
}

const uint8_t *tbft_cr_load(const tbft_checkpoint_region_t *cr,
                             tbft_seqno_t seqno,
                             tbft_node_id_t replica_id, int *len_out)
{
    if (replica_id < 0 || replica_id >= TBFT_MAX_NUM_REPLICAS) {
        if (len_out) *len_out = 0;
        return nullptr;
    }
    int sidx = slot_index(seqno);
    const tbft_ckpt_slot_t *slot = &cr->slots[sidx];
    if (slot->seqno != seqno || !slot->present[replica_id]) {
        if (len_out) *len_out = 0;
        return nullptr;
    }
    if (len_out) *len_out = slot->msg_lens[replica_id];
    return slot->msgs[replica_id];
}

int tbft_cr_count(const tbft_checkpoint_region_t *cr, tbft_seqno_t seqno)
{
    int sidx = slot_index(seqno);
    const tbft_ckpt_slot_t *slot = &cr->slots[sidx];
    if (slot->seqno != seqno) return 0;
    return slot->match_count;
}

const tbft_digest_t *tbft_cr_winning_digest(const tbft_checkpoint_region_t *cr,
                                             tbft_seqno_t seqno)
{
    const tbft_ckpt_slot_t *slot = &cr->slots[slot_index(seqno)];
    /* The slot may hold another seqno that maps to the same index. */
    if (slot->seqno != seqno || slot->match_count < cr->threshold) return nullptr;
    return &slot->winning_digest;
}

void tbft_cr_truncate(tbft_checkpoint_region_t *cr, tbft_seqno_t stable_seqno)
{
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        tbft_ckpt_slot_t *slot = &cr->slots[i];
        if (slot->seqno <= stable_seqno) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}
