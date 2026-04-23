#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_certificate.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Prepared_cert — combines a Pre_prepare with a Certificate<Prepare>.
 *
 * Complete when:
 *   1. pp_buf is valid (pre-prepare received)
 *   2. pc.is_complete() (at least 2f matching prepares)
 *   3. Pre_prepare digest matches pc.cvalue() digest
 * -------------------------------------------------------------------------- */

/* Maximum size for an in-memory pre-prepare (header + request set + auth) */
#define TBFT_PP_MAX_SIZE  TBFT_MAX_MESSAGE_SIZE

typedef struct tbft_prepared_cert {
    /* Pre-prepare storage */
    uint8_t  pp_buf[TBFT_PP_MAX_SIZE];
    int      pp_len;   /* 0 = no pre-prepare received */

    /* Prepare certificate */
    tbft_prepare_cert_t  pc;

    /* Time the pre-prepare was stored (for retransmission logic) */
    int64_t  t_sent_us;
} tbft_prepared_cert_t;

/* --------------------------------------------------------------------------
 * Operations
 * -------------------------------------------------------------------------- */

void tbft_prepared_cert_init(tbft_prepared_cert_t *cert, int prepare_threshold);

void tbft_prepared_cert_clear(tbft_prepared_cert_t *cert);

/**
 * Store a received Pre_prepare.
 * @return true if accepted (no pre-prepare was already stored)
 */
bool tbft_prepared_cert_add_pp(tbft_prepared_cert_t *cert,
                               const void *pp_buf, int pp_len);

/**
 * Add a Prepare from @p sender_id.
 * @return true if accepted
 */
bool tbft_prepared_cert_add_prepare(tbft_prepared_cert_t *cert,
                                    const void *msg, int msg_len,
                                    tbft_node_id_t sender_id);

/**
 * Add this node's own Prepare.
 */
bool tbft_prepared_cert_add_my_prepare(tbft_prepared_cert_t *cert,
                                       const void *msg, int msg_len,
                                       tbft_node_id_t my_id);

/**
 * Returns true when the certificate is complete:
 *  - pre-prepare present
 *  - prepare cert complete (2f matching prepares)
 *  - digests match
 */
bool tbft_prepared_cert_is_complete(const tbft_prepared_cert_t *cert);

/**
 * Returns a pointer to the stored Pre_prepare bytes, or NULL.
 */
static inline const uint8_t *tbft_prepared_cert_pp(
        const tbft_prepared_cert_t *cert)
{
    return cert->pp_len > 0 ? cert->pp_buf : NULL;
}

/**
 * Returns a pointer to the stored Pre_prepare rep, or NULL.
 */
static inline const tbft_pre_prepare_rep_t *tbft_prepared_cert_pp_rep(
        const tbft_prepared_cert_t *cert)
{
    return cert->pp_len > 0
           ? (const tbft_pre_prepare_rep_t *)cert->pp_buf
           : NULL;
}

/* --------------------------------------------------------------------------
 * NOTE: tbft_plog_t (Log<Prepared_cert>) was superseded by
 * tbft_agreement_region_t which serves the same purpose with better
 * memory layout. The plog type and its functions are no longer used.
 * -------------------------------------------------------------------------- */
