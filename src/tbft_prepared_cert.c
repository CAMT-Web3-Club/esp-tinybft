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
        /* Already have a PP stored.  Accept re-proposal from a new view
         * if the request-set digest matches — same request, new view. */
        const tbft_pre_prepare_rep_t *old =
            (const tbft_pre_prepare_rep_t *)cert->pp_buf;
        const tbft_pre_prepare_rep_t *new_pp =
            (const tbft_pre_prepare_rep_t *)pp_buf;
        if (pp_len >= (int)sizeof(tbft_pre_prepare_rep_t) &&
            old->seqno == new_pp->seqno &&
            tbft_digest_equal(&old->digest, &new_pp->digest) &&
            new_pp->view > old->view) {
            /* Accept re-proposal: overwrite with new view's PP */
            memcpy(cert->pp_buf, pp_buf, (size_t)pp_len);
            cert->pp_len    = pp_len;
            cert->t_sent_us = esp_timer_get_time();
            return true;
        }
        return false; /* different request or same view — reject */
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
 * NOTE: tbft_plog_init and tbft_plog_truncate were superseded by
 * tbft_agreement_region_t and are no longer used. Implementations removed.
 * -------------------------------------------------------------------------- */
