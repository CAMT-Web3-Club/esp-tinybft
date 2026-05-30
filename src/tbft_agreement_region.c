#include "tbft_agreement_region.h"
#include <string.h>

void tbft_ar_init(tbft_agreement_region_t *ar,
                  int prepare_threshold, int commit_threshold)
{
    memset(ar, 0, sizeof(*ar));
    ar->head              = 1;
    ar->head_idx          = 0;
    ar->mask              = TBFT_WINDOW_SIZE - 1;
    ar->prepare_threshold = prepare_threshold;
    ar->commit_threshold  = commit_threshold;

    for (int i = 0; i < TBFT_WINDOW_SIZE; i++) {
        tbft_prepared_cert_init(&ar->slices[i].prepared_cert,
                                prepare_threshold);
        tbft_commit_cert_init(&ar->slices[i].commit_cert, commit_threshold);
    }
}

/* --------------------------------------------------------------------------
 * Pre_prepare
 * -------------------------------------------------------------------------- */

bool tbft_ar_store_pp(tbft_agreement_region_t *ar,
                      tbft_seqno_t seqno,
                      const void *pp_buf, int pp_len)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl = tbft_ar_slice(ar, seqno);
    return tbft_prepared_cert_add_pp(&sl->prepared_cert, pp_buf, pp_len);
}

const uint8_t *tbft_ar_load_pp(const tbft_agreement_region_t *ar,
                               tbft_seqno_t seqno, int *len_out)
{
    if (!tbft_ar_in_range(ar, seqno)) {
        if (len_out) *len_out = 0;
        return NULL;
    }
    const tbft_agreement_slice_t *sl =
        (const tbft_agreement_slice_t *)tbft_ar_slice(
            (tbft_agreement_region_t *)ar, seqno);
    if (len_out) *len_out = sl->prepared_cert.pp_len;
    return tbft_prepared_cert_pp(&sl->prepared_cert);
}

/* --------------------------------------------------------------------------
 * Prepare
 * -------------------------------------------------------------------------- */

bool tbft_ar_add_prepare(tbft_agreement_region_t *ar,
                         tbft_seqno_t seqno,
                         const void *msg, int msg_len,
                         tbft_node_id_t sender_id)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl = tbft_ar_slice(ar, seqno);
    return tbft_prepared_cert_add_prepare(&sl->prepared_cert,
                                          msg, msg_len, sender_id);
}

bool tbft_ar_add_my_prepare(tbft_agreement_region_t *ar,
                            tbft_seqno_t seqno,
                            const void *msg, int msg_len,
                            tbft_node_id_t my_id)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl = tbft_ar_slice(ar, seqno);
    return tbft_prepared_cert_add_my_prepare(&sl->prepared_cert,
                                              msg, msg_len, my_id);
}

bool tbft_ar_prepared(const tbft_agreement_region_t *ar, tbft_seqno_t seqno)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl =
        tbft_ar_slice((tbft_agreement_region_t *)ar, seqno);
    return tbft_prepared_cert_is_complete(&sl->prepared_cert);
}

/* --------------------------------------------------------------------------
 * Commit
 * -------------------------------------------------------------------------- */

bool tbft_ar_add_commit(tbft_agreement_region_t *ar,
                        tbft_seqno_t seqno,
                        const void *msg, int msg_len,
                        tbft_node_id_t sender_id)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl = tbft_ar_slice(ar, seqno);
    return tbft_commit_cert_add(&sl->commit_cert, msg, msg_len, sender_id);
}

bool tbft_ar_add_my_commit(tbft_agreement_region_t *ar,
                           tbft_seqno_t seqno,
                           const void *msg, int msg_len,
                           tbft_node_id_t my_id)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl = tbft_ar_slice(ar, seqno);
    return tbft_commit_cert_add_mine(&sl->commit_cert, msg, msg_len, my_id);
}

bool tbft_ar_committed(const tbft_agreement_region_t *ar, tbft_seqno_t seqno)
{
    if (!tbft_ar_in_range(ar, seqno)) return false;
    tbft_agreement_slice_t *sl =
        tbft_ar_slice((tbft_agreement_region_t *)ar, seqno);
    return tbft_commit_cert_is_complete(&sl->commit_cert);
}

/* --------------------------------------------------------------------------
 * Truncation
 * -------------------------------------------------------------------------- */

void tbft_ar_truncate(tbft_agreement_region_t *ar, tbft_seqno_t new_head)
{
    if (new_head <= ar->head) return;

    tbft_seqno_t old_head   = ar->head;
    tbft_seqno_t full_delta = new_head - old_head;

    /* Cap the CLEAR loop to WINDOW_SIZE to prevent iterating billions
     * of times on a large fetch jump.  The circular buffer only has
     * WINDOW_SIZE slots, so clearing all of them is sufficient. */
    tbft_seqno_t delta = full_delta;
    if (delta > TBFT_WINDOW_SIZE) delta = TBFT_WINDOW_SIZE;

    for (tbft_seqno_t i = 0; i < delta; i++) {
        int idx = (int)((ar->head_idx + (int)i) & ar->mask);
        tbft_prepared_cert_clear(&ar->slices[idx].prepared_cert);
        tbft_commit_cert_clear(&ar->slices[idx].commit_cert);
        ar->slices[idx].commit_sent_us = 0;
    }

    /* Advance the circular pointer by the FULL jump so the head lands
     * at new_head.  Using the capped delta here (ar->head + delta)
     * would leave the head far behind new_head when the jump exceeds
     * the window, causing tbft_ar_in_range() to permanently reject all
     * consensus messages. */
    ar->head_idx = (int)((ar->head_idx + (int)full_delta) & ar->mask);
    ar->head     = new_head;
}
