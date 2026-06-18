#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_certificate.h"
#include "tbft_prepared_cert.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * agreement_region — TinyBFT static circular buffer for the hot-path
 * agreement messages (section 7).
 *
 * Replaces dynamic heap allocation of Pre_prepare, Prepare, and Commit
 * messages.  Holds TBFT_WINDOW_SIZE AgreementSlice entries, each containing:
 *   - One Prepared_cert (1 Pre_prepare + f+1 Prepare slots)
 *   - One CommitCert   (f+1 Commit slots)
 *
 * Indexed by sequence number: slice_index(n) = (n - head) % WINDOW_SIZE
 * -------------------------------------------------------------------------- */

/** One slot in the agreement region */
typedef struct {
    tbft_prepared_cert_t  prepared_cert;
    tbft_commit_cert_t    commit_cert;
    int64_t               commit_sent_us; /* Last time we transmitted our commit for this slice */
    int64_t               fill_sent_us;   /* Last time we sent a fill-request for this slice */
} tbft_agreement_slice_t;

/** The agreement region (statically allocated) */
typedef struct {
    tbft_agreement_slice_t  slices[TBFT_WINDOW_SIZE];
    tbft_seqno_t            head;      /* lowest live seqno */
    int                     head_idx;  /* circular index for head */
    int                     mask;      /* TBFT_WINDOW_SIZE - 1 */
    int                     prepare_threshold; /* 2f */
    int                     commit_threshold;  /* 2f+1 */
} tbft_agreement_region_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise the agreement region.
 * @param ar               Region to initialise
 * @param prepare_threshold  Prepare quorum (2f — only 2f prepares needed
 *                           since the primary's pre-prepare counts)
 * @param commit_threshold   Commit quorum (2f+1)
 */
void tbft_ar_init(tbft_agreement_region_t *ar,
                  int prepare_threshold, int commit_threshold);

/* --------------------------------------------------------------------------
 * Slice access (indexed by sequence number)
 * -------------------------------------------------------------------------- */

/**
 * Return true if @p n is within the current window.
 */
static inline bool tbft_ar_in_range(const tbft_agreement_region_t *ar,
                                    tbft_seqno_t n)
{
    return (n >= ar->head) && (n < ar->head + TBFT_WINDOW_SIZE);
}

/**
 * Get the slice for sequence number @p n.
 * Caller must check tbft_ar_in_range() first.
 */
static inline tbft_agreement_slice_t *tbft_ar_slice(
        tbft_agreement_region_t *ar, tbft_seqno_t n)
{
    int idx = (int)((ar->head_idx + (n - ar->head)) & ar->mask);
    return &ar->slices[idx];
}

/* --------------------------------------------------------------------------
 * Pre_prepare operations
 * -------------------------------------------------------------------------- */

/**
 * Store a received (or self-generated) Pre_prepare into the region.
 * @return true if stored successfully
 */
bool tbft_ar_store_pp(tbft_agreement_region_t *ar,
                      tbft_seqno_t seqno,
                      const void *pp_buf, int pp_len);

/**
 * Load the stored Pre_prepare for @p seqno.
 * @return pointer to the pre-prepare bytes, or NULL
 */
const uint8_t *tbft_ar_load_pp(const tbft_agreement_region_t *ar,
                               tbft_seqno_t seqno, int *len_out);

/* --------------------------------------------------------------------------
 * Prepare operations
 * -------------------------------------------------------------------------- */

/**
 * Add a Prepare message from @p sender_id to the slot for @p seqno.
 */
bool tbft_ar_add_prepare(tbft_agreement_region_t *ar,
                         tbft_seqno_t seqno,
                         const void *msg, int msg_len,
                         tbft_node_id_t sender_id);

/**
 * Add this node's own Prepare to the slot for @p seqno.
 */
bool tbft_ar_add_my_prepare(tbft_agreement_region_t *ar,
                            tbft_seqno_t seqno,
                            const void *msg, int msg_len,
                            tbft_node_id_t my_id);

/**
 * True when the prepared cert for @p seqno is complete.
 */
bool tbft_ar_prepared(const tbft_agreement_region_t *ar, tbft_seqno_t seqno);

/* --------------------------------------------------------------------------
 * Commit operations
 * -------------------------------------------------------------------------- */

/**
 * Add a Commit message from @p sender_id to the slot for @p seqno.
 */
bool tbft_ar_add_commit(tbft_agreement_region_t *ar,
                        tbft_seqno_t seqno,
                        const void *msg, int msg_len,
                        tbft_node_id_t sender_id);

/**
 * Add this node's own Commit to the slot for @p seqno.
 */
bool tbft_ar_add_my_commit(tbft_agreement_region_t *ar,
                           tbft_seqno_t seqno,
                           const void *msg, int msg_len,
                           tbft_node_id_t my_id);

/**
 * True when the commit cert for @p seqno is complete (committed-local).
 */
bool tbft_ar_committed(const tbft_agreement_region_t *ar, tbft_seqno_t seqno);

/* --------------------------------------------------------------------------
 * Garbage collection
 * -------------------------------------------------------------------------- */

/**
 * Advance the window head to @p new_head, clearing freed slots.
 */
void tbft_ar_truncate(tbft_agreement_region_t *ar, tbft_seqno_t new_head);

