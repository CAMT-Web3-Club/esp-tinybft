#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include <string.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Certificate<T> — quorum certificate (section 6).
 *
 * Collects at most one message per sender.  Groups identical messages and
 * becomes COMPLETE once `complete_threshold` (default 2f+1) matching
 * messages have been received.
 *
 * In C we represent the generic T by storing raw message bytes.  Separate
 * typed wrappers (tbft_commit_cert_t, tbft_checkpoint_cert_t) are provided
 * for each usage to carry the appropriate buffer size.
 *
 * Maximum distinct values stored = f+1 (beyond that a quorum is guaranteed).
 * -------------------------------------------------------------------------- */

/* Maximum bytes for one certificate entry:
 *   Commit:     sizeof(tbft_commit_rep_t)     + TBFT_AUTH_SIZE
 *   Checkpoint: sizeof(tbft_checkpoint_rep_t) + TBFT_AUTH_SIZE
 *   Prepare:    sizeof(tbft_prepare_rep_t)    + TBFT_AUTH_SIZE
 * Use the largest across all three.
 */
#define TBFT_CERT_COMMIT_MSG_SIZE     \
    (sizeof(tbft_commit_rep_t) + TBFT_AUTH_SIZE)

#define TBFT_CERT_CHECKPOINT_MSG_SIZE \
    (sizeof(tbft_checkpoint_rep_t) + TBFT_AUTH_SIZE)

#define TBFT_CERT_PREPARE_MSG_SIZE    \
    (sizeof(tbft_prepare_rep_t) + TBFT_AUTH_SIZE)

/* --------------------------------------------------------------------------
 * Generic certificate structure
 * Callers instantiate a concrete version (see typedefs below).
 * -------------------------------------------------------------------------- */

#define TBFT_CERT_DECLARE(name, msg_size)                                      \
typedef struct {                                                               \
    tbft_bitmap_t  bmap;              /* which sender IDs have contributed */  \
    tbft_digest_t  val_digests[TBFT_CERT_MAX_VALS]; /* digests for comparison */ \
    uint8_t        vals[TBFT_CERT_MAX_VALS][(msg_size)]; /* stored msgs */     \
    int            correct[TBFT_CERT_MAX_VALS]; /* match count per value */    \
    int            num_vals;          /* distinct values stored so far */      \
    int            mym_idx;           /* index of own message, -1 if none */   \
    int            complete_threshold;/* threshold to be complete (2f+1) */   \
} name

TBFT_CERT_DECLARE(tbft_commit_cert_t,     TBFT_CERT_COMMIT_MSG_SIZE);
TBFT_CERT_DECLARE(tbft_checkpoint_cert_t, TBFT_CERT_CHECKPOINT_MSG_SIZE);
TBFT_CERT_DECLARE(tbft_prepare_cert_t,    TBFT_CERT_PREPARE_MSG_SIZE);

/* --------------------------------------------------------------------------
 * Generic certificate operations (operate on the common prefix of all certs)
 * -------------------------------------------------------------------------- */

/**
 * Internal layout shared by all TBFT_CERT_DECLARE structs:
 * The first field is bmap, second is vals (opaque from here), then
 * correct[], num_vals, mym_idx, complete_threshold.
 *
 * We define a "raw" view for the common-prefix operations.
 */
typedef struct {
    tbft_bitmap_t  bmap;
    /* vals and correct are accessed via pointer arithmetic by callers */
    int            correct_dummy[1]; /* placeholder */
    int            num_vals;
    int            mym_idx;
    int            complete_threshold;
} tbft_cert_common_t;

/* --------------------------------------------------------------------------
 * Typed add / query operations
 *
 * These operate directly on the concrete types to avoid void* games.
 * Each takes:
 *   cert         — pointer to the cert
 *   msg          — raw message bytes
 *   msg_len      — length of message (must be <= slot size)
 *   sender_id    — sender's node id
 *   my_node_id   — this node's id (for add_mine)
 * -------------------------------------------------------------------------- */

/**
 * Try to add a message from @p sender_id to a commit certificate.
 * @return true if the message was accepted (not a duplicate sender)
 */
bool tbft_commit_cert_add(tbft_commit_cert_t *cert,
                          const void *msg, int msg_len,
                          tbft_node_id_t sender_id);

/** Add this node's own commit message to the certificate. */
bool tbft_commit_cert_add_mine(tbft_commit_cert_t *cert,
                               const void *msg, int msg_len,
                               tbft_node_id_t my_id);

/** Return pointer to the winning value (correct >= threshold), or NULL. */
const uint8_t *tbft_commit_cert_cvalue(const tbft_commit_cert_t *cert);

/** True when the certificate has reached its completion threshold. */
bool tbft_commit_cert_is_complete(const tbft_commit_cert_t *cert);

/** Reset the certificate to empty. */
void tbft_commit_cert_clear(tbft_commit_cert_t *cert);

/** Initialise with given quorum threshold. */
void tbft_commit_cert_init(tbft_commit_cert_t *cert, int threshold);


/* Same operations for Checkpoint certificates */
bool        tbft_checkpoint_cert_add(tbft_checkpoint_cert_t *cert,
                                     const void *msg, int msg_len,
                                     tbft_node_id_t sender_id);
bool        tbft_checkpoint_cert_add_mine(tbft_checkpoint_cert_t *cert,
                                          const void *msg, int msg_len,
                                          tbft_node_id_t my_id);
const uint8_t *tbft_checkpoint_cert_cvalue(const tbft_checkpoint_cert_t *cert);
bool        tbft_checkpoint_cert_is_complete(const tbft_checkpoint_cert_t *cert);
void        tbft_checkpoint_cert_clear(tbft_checkpoint_cert_t *cert);
void        tbft_checkpoint_cert_init(tbft_checkpoint_cert_t *cert, int threshold);


/* Same operations for Prepare certificates */
bool        tbft_prepare_cert_add(tbft_prepare_cert_t *cert,
                                  const void *msg, int msg_len,
                                  tbft_node_id_t sender_id);
bool        tbft_prepare_cert_add_mine(tbft_prepare_cert_t *cert,
                                       const void *msg, int msg_len,
                                       tbft_node_id_t my_id);
const uint8_t *tbft_prepare_cert_cvalue(const tbft_prepare_cert_t *cert);
bool        tbft_prepare_cert_is_complete(const tbft_prepare_cert_t *cert);
void        tbft_prepare_cert_clear(tbft_prepare_cert_t *cert);
void        tbft_prepare_cert_init(tbft_prepare_cert_t *cert, int threshold);
