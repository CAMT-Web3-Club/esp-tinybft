#include "tbft_prepared_cert.h"
#include "esp_timer.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Prepared_cert
 * -------------------------------------------------------------------------- */

void tbft_prepared_cert_init(tbft_prepared_cert_t *cert, int prepare_threshold)
{
    memset(cert, 0, sizeof(*cert));
    tbft_prepare_cert_init(&cert->pc, prepare_threshold);
}

void tbft_prepared_cert_clear(tbft_prepared_cert_t *cert)
{
    int thr = cert->pc.complete_threshold;
    memset(cert, 0, sizeof(*cert));
    tbft_prepare_cert_init(&cert->pc, thr);
}

bool tbft_prepared_cert_add_pp(tbft_prepared_cert_t *cert,
                               const void *pp_buf, int pp_len)
{
    if (cert->pp_len > 0) {
        return false; /* already have a pre-prepare */
    }
    if (pp_len < (int)sizeof(tbft_pre_prepare_rep_t)) {
        return false; /* shorter than the fixed header — and guards negatives */
    }
    if (pp_len > TBFT_PP_MAX_SIZE) {
        return false; /* too large */
    }
    memcpy(cert->pp_buf, pp_buf, (size_t)pp_len);
    cert->pp_len    = pp_len;
    cert->t_sent_us = esp_timer_get_time();
    return true;
}

bool tbft_prepared_cert_add_prepare(tbft_prepared_cert_t *cert,
                                    const void *msg, int msg_len,
                                    tbft_node_id_t sender_id)
{
    return tbft_prepare_cert_add(&cert->pc, msg, msg_len, sender_id);
}

bool tbft_prepared_cert_add_my_prepare(tbft_prepared_cert_t *cert,
                                       const void *msg, int msg_len,
                                       tbft_node_id_t my_id)
{
    return tbft_prepare_cert_add_mine(&cert->pc, msg, msg_len, my_id);
}

bool tbft_prepared_cert_is_complete(const tbft_prepared_cert_t *cert)
{
    if (cert->pp_len == 0) return false;
    if (!tbft_prepare_cert_is_complete(&cert->pc)) return false;

    /* Verify that the winning prepare's digest matches the pre-prepare */
    const uint8_t *pval = tbft_prepare_cert_cvalue(&cert->pc);
    if (!pval) return false;

    const tbft_pre_prepare_rep_t *pp =
        (const tbft_pre_prepare_rep_t *)cert->pp_buf;
    const tbft_prepare_rep_t *prep = (const tbft_prepare_rep_t *)pval;

    return tbft_digest_equal(&pp->digest, &prep->digest);
}

/* --------------------------------------------------------------------------
 * Prepare log
 * -------------------------------------------------------------------------- */

void tbft_plog_init(tbft_plog_t *log, int prepare_threshold)
{
    memset(log, 0, sizeof(*log));
    log->head             = 1;
    log->head_idx         = 0;
    log->mask             = TBFT_WINDOW_SIZE - 1;
    log->prepare_threshold = prepare_threshold;
    for (int i = 0; i < TBFT_WINDOW_SIZE; i++) {
        tbft_prepared_cert_init(&log->slots[i], prepare_threshold);
    }
}

void tbft_plog_truncate(tbft_plog_t *log, tbft_seqno_t new_head)
{
    if (new_head <= log->head) return; /* nothing to truncate, avoids negative cast */

    /* Cap the loop to at most one full window to prevent billion-iteration
     * stalls on pathological jumps.  Entries beyond the cap are cleared
     * implicitly: they will be overwritten by new certificates once their
     * slot index wraps back, and the stale data is harmless because the
     * prepare threshold will not be met for a different seqno. */
    tbft_seqno_t delta = new_head - log->head;
    if (delta > TBFT_WINDOW_SIZE) delta = TBFT_WINDOW_SIZE;

    for (tbft_seqno_t s = 0; s < delta; s++) {
        int idx = (int)((log->head_idx + s) & log->mask);
        tbft_prepared_cert_clear(&log->slots[idx]);
    }
    log->head_idx = (int)((log->head_idx + (new_head - log->head))
                          & log->mask);
    log->head = new_head;
}
