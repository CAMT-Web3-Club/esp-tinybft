#pragma once

#include "tbft_types.h"
#include "tbft_certificate.h"
#include <stdbool.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Log<T> — circular buffer indexed by sequence number (section 6).
 *
 * Holds TBFT_WINDOW_SIZE slots.  Access uses (seqno & mask) for O(1) index.
 * The head is the lowest live sequence number (inclusive).
 * truncate(new_head) advances the head and clears freed slots.
 *
 * We define two concrete Log types needed by the Replica:
 *   tbft_plog_t — Log<Prepared_cert>  (prepare log)
 *   tbft_clog_t — Log<CommitCert>     (commit log)
 *   tbft_elog_t — CheckpointLog       (checkpoint log)
 * -------------------------------------------------------------------------- */

/* Forward-declare the types to avoid circular includes */
typedef struct tbft_prepared_cert tbft_prepared_cert_t;

/* --------------------------------------------------------------------------
 * Commit log  (Log<tbft_commit_cert_t>)
 * -------------------------------------------------------------------------- */

typedef struct {
    tbft_commit_cert_t  slots[TBFT_WINDOW_SIZE];
    tbft_seqno_t        head;        /* first live seqno */
    int                 head_idx;    /* circular index for head */
    int                 mask;        /* TBFT_WINDOW_SIZE - 1 */
    int                 threshold;   /* 2f+1, stored once at init */
} tbft_clog_t;

static inline void tbft_clog_init(tbft_clog_t *log, int threshold)
{
    memset(log, 0, sizeof(*log));
    log->head      = 1;
    log->head_idx  = 0;
    log->mask      = TBFT_WINDOW_SIZE - 1;
    log->threshold = threshold;
    for (int i = 0; i < TBFT_WINDOW_SIZE; i++) {
        tbft_commit_cert_init(&log->slots[i], threshold);
    }
}

static inline bool tbft_clog_in_range(const tbft_clog_t *log, tbft_seqno_t n)
{
    return (n > log->head - 1) && (n <= log->head + TBFT_WINDOW_SIZE - 1);
}

static inline tbft_commit_cert_t *tbft_clog_get(tbft_clog_t *log,
                                                 tbft_seqno_t n)
{
    int idx = (int)((log->head_idx + (n - log->head)) & log->mask);
    return &log->slots[idx];
}

static inline void tbft_clog_truncate(tbft_clog_t *log, tbft_seqno_t new_head)
{
    for (tbft_seqno_t s = log->head; s < new_head; s++) {
        int idx = (int)((log->head_idx + (s - log->head)) & log->mask);
        tbft_commit_cert_clear(&log->slots[idx]);
    }
    log->head_idx = (int)((log->head_idx + (int)(new_head - log->head))
                          & log->mask);
    log->head = new_head;
}

/* --------------------------------------------------------------------------
 * Checkpoint log  (tbft_elog_t)
 * Uses fewer slots: TBFT_NUM_CKPT_SLOTS, indexed by checkpoint seqno.
 * -------------------------------------------------------------------------- */

typedef struct {
    tbft_checkpoint_cert_t  slots[TBFT_NUM_CKPT_SLOTS];
    tbft_seqno_t            head;
    int                     threshold;
    int                     num_slots;
} tbft_elog_t;

static inline void tbft_elog_init(tbft_elog_t *log, int threshold)
{
    memset(log, 0, sizeof(*log));
    log->threshold = threshold;
    log->num_slots = TBFT_NUM_CKPT_SLOTS;
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        tbft_checkpoint_cert_init(&log->slots[i], threshold);
    }
}

static inline tbft_checkpoint_cert_t *tbft_elog_get(tbft_elog_t *log,
                                                     tbft_seqno_t seqno)
{
    int slot = (int)((seqno / TBFT_CHECKPOINT_INTERVAL) % log->num_slots);
    return &log->slots[slot];
}

static inline void tbft_elog_truncate(tbft_elog_t *log, tbft_seqno_t new_head)
{
    /* Clear slots for seqnos below new_head */
    for (tbft_seqno_t s = log->head;
         s < new_head;
         s += TBFT_CHECKPOINT_INTERVAL) {
        tbft_checkpoint_cert_t *c = tbft_elog_get(log, s);
        tbft_checkpoint_cert_clear(c);
    }
    log->head = new_head;
}
