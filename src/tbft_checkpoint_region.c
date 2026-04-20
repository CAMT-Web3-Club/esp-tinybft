#include "tbft_checkpoint_region.h"
#include <string.h>

static inline int slot_index(tbft_seqno_t seqno)
{
    if (seqno < 0) return 0; /* guard negative seqno from Kconfig misuse */
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
    if (msg_len < (int)sizeof(tbft_checkpoint_rep_t) ||
        msg_len > (int)TBFT_CKPT_MSG_SIZE) return;

    tbft_ckpt_slot_t *slot = &cr->above_window[replica_id];

    /* Only advance if the incoming message is for a strictly newer checkpoint
     * than what we already have from this replica.  This prevents a Byzantine
     * (or duplicate-legitimate) peer from spamming above-window checkpoints
     * and silently overwriting useful earlier state. */
    const tbft_checkpoint_rep_t *rep = (const tbft_checkpoint_rep_t *)msg;
    if (slot->present[0] && rep->seqno <= slot->seqno) {
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->seqno = rep->seqno;
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
        tbft_ckpt_slot_t *slot = &cr->slots[i];
        if (slot->seqno <= stable_seqno) {
            memset(slot, 0, sizeof(*slot));
        }
    }
    /* Also clear any above-window entry that is now at-or-below the stable
     * point: it is no longer "above" anything and must not alias a future
     * slot store. */
    for (int i = 0; i < TBFT_MAX_NUM_REPLICAS; i++) {
        tbft_ckpt_slot_t *slot = &cr->above_window[i];
        if (slot->present[0] && slot->seqno <= stable_seqno) {
            memset(slot, 0, sizeof(*slot));
        }
    }
    /* Also clear any above-window entry that is now at-or-below the stable
     * point: it is no longer "above" anything and must not alias a future
     * slot store. */
    for (int i = 0; i < TBFT_MAX_NUM_REPLICAS; i++) {
        tbft_ckpt_slot_t *slot = &cr->above_window[i];
        if (slot->present[0] && slot->seqno <= stable_seqno) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}
