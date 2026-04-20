#include "tbft_certificate.h"
#include "tbft_message.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Generic certificate logic (macro-expanded for each concrete type).
 *
 * All three certificate types (commit, checkpoint, prepare) follow identical
 * logic; only the slot size and type name differ.
 *
 * CRITICAL FIX: Instead of comparing entire messages (which include sender-
 * specific fields like `id` and per-sender authenticators), we now compare
 * only the digest portion of each message type. This allows certificates to
 * reach quorum when different senders agree on the same value.
 * -------------------------------------------------------------------------- */

/* Extract digest pointer from a message based on its type.
 * Returns NULL if the message is too small. */
static const tbft_digest_t *extract_msg_digest(const void *msg, int msg_len,
                                                tbft_msg_tag_t expected_tag,
                                                size_t digest_offset)
{
    if (!msg || msg_len <= 0) {
        return NULL;
    }
    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    if ((size_t)msg_len < digest_offset + sizeof(tbft_digest_t)) {
        return NULL;
    }
    if (hdr->tag != expected_tag) {
        return NULL;
    }
    return (const tbft_digest_t *)((const uint8_t *)msg + digest_offset);
}

#define CERT_IMPL(prefix, cert_t, msg_slot_size, digest_offset, msg_tag)       \
                                                                               \
void prefix##_init(cert_t *cert, int threshold)                                \
{                                                                              \
    memset(cert, 0, sizeof(*cert));                                            \
    cert->mym_idx            = -1;                                             \
    cert->complete_threshold = threshold;                                      \
}                                                                              \
                                                                               \
void prefix##_clear(cert_t *cert)                                              \
{                                                                              \
    int thr = cert->complete_threshold;                                        \
    memset(cert, 0, sizeof(*cert));                                            \
    cert->mym_idx            = -1;                                             \
    cert->complete_threshold = thr;                                            \
}                                                                              \
                                                                               \
bool prefix##_add(cert_t *cert, const void *msg, int msg_len,                  \
                  tbft_node_id_t sender_id)                                    \
{                                                                              \
    if (msg_len <= 0 || (size_t)msg_len > (msg_slot_size)) {                   \
        return false; /* reject negative / oversized — no silent truncation */ \
    }                                                                          \
    if (tbft_bitmap_test(&cert->bmap, sender_id)) {                            \
        return false; /* already have from this sender */                      \
    }                                                                          \
    /* Extract the digest from this message */                                 \
    const tbft_digest_t *msg_digest =                                          \
        extract_msg_digest(msg, msg_len, msg_tag, digest_offset);              \
    if (!msg_digest) {                                                         \
        return false; /* invalid message */                                    \
    }                                                                          \
    /* Check if this message's digest matches an existing value */             \
    for (int i = 0; i < cert->num_vals; i++) {                                 \
        const tbft_digest_t *stored_digest =                                   \
            &cert->val_digests[i];                                           \
        if (tbft_digest_equal(msg_digest, stored_digest)) {                    \
            tbft_bitmap_set(&cert->bmap, sender_id);                           \
            cert->correct[i]++;                                                \
            return true;                                                       \
        }                                                                      \
    }                                                                          \
    /* New distinct value — only store up to f+1 */                            \
    if (cert->num_vals >= TBFT_CERT_MAX_VALS) {                                \
        return false; /* no room */                                            \
    }                                                                          \
    /* Store the digest for comparison */                                      \
    memcpy(&cert->val_digests[cert->num_vals], msg_digest,                     \
           sizeof(tbft_digest_t));                                             \
    /* Also store the full message for retrieval (bounded by slot size) */    \
    memcpy(cert->vals[cert->num_vals], msg, (size_t)msg_len);                  \
    cert->correct[cert->num_vals] = 1;                                         \
    tbft_bitmap_set(&cert->bmap, sender_id);                                   \
    cert->num_vals++;                                                          \
    return true;                                                               \
}                                                                              \
                                                                               \
bool prefix##_add_mine(cert_t *cert, const void *msg, int msg_len,             \
                       tbft_node_id_t my_id)                                   \
{                                                                              \
    if (msg_len <= 0 || (size_t)msg_len > (msg_slot_size)) {                   \
        return false; /* reject negative / oversized — no silent truncation */ \
    }                                                                          \
    if (tbft_bitmap_test(&cert->bmap, my_id)) {                                \
        return false;                                                          \
    }                                                                          \
    const tbft_digest_t *msg_digest =                                          \
        extract_msg_digest(msg, msg_len, msg_tag, digest_offset);              \
    if (!msg_digest) {                                                         \
        return false;                                                          \
    }                                                                          \
    /* Store as first value if slot available */                               \
    if (cert->num_vals < TBFT_CERT_MAX_VALS) {                                 \
        memcpy(&cert->val_digests[cert->num_vals], msg_digest,                 \
               sizeof(tbft_digest_t));                                         \
        memcpy(cert->vals[cert->num_vals], msg, (size_t)msg_len);              \
        cert->correct[cert->num_vals] = 1;                                     \
        cert->mym_idx = cert->num_vals;                                        \
        cert->num_vals++;                                                      \
    } else {                                                                   \
        /* Certificate is full — we cannot store our own value.  Return false  \
         * so the caller knows the vote was not recorded.  Do NOT set the      \
         * bitmap: a marked-but-absent vote would make cvalue() unable to      \
         * recover the local value, breaking quorum formation. */              \
        return false;                                                          \
    }                                                                          \
    tbft_bitmap_set(&cert->bmap, my_id);                                       \
    return true;                                                               \
}                                                                              \
                                                                               \
const uint8_t *prefix##_cvalue(const cert_t *cert)                            \
{                                                                              \
    for (int i = 0; i < cert->num_vals; i++) {                                 \
        if (cert->correct[i] >= cert->complete_threshold) {                    \
            return cert->vals[i];                                              \
        }                                                                      \
    }                                                                          \
    return NULL;                                                               \
}                                                                              \
                                                                               \
bool prefix##_is_complete(const cert_t *cert)                                  \
{                                                                              \
    return prefix##_cvalue(cert) != NULL;                                      \
}

/* Expand for each type with correct digest offsets:
 * - Prepare: digest is at offset sizeof(tbft_msg_hdr_t) + sizeof(tbft_view_t) + sizeof(tbft_seqno_t)
 * - Commit: digest is at offset sizeof(tbft_msg_hdr_t) + sizeof(tbft_view_t) + sizeof(tbft_seqno_t)
 * - Checkpoint: digest is at offset sizeof(tbft_msg_hdr_t) + sizeof(tbft_seqno_t)
 */
#define PREPARE_DIGEST_OFFSET (sizeof(tbft_msg_hdr_t) + sizeof(tbft_view_t) + sizeof(tbft_seqno_t))
#define COMMIT_DIGEST_OFFSET  (sizeof(tbft_msg_hdr_t) + sizeof(tbft_view_t) + sizeof(tbft_seqno_t))
#define CHECKPOINT_DIGEST_OFFSET (sizeof(tbft_msg_hdr_t) + sizeof(tbft_seqno_t))

CERT_IMPL(tbft_commit_cert,     tbft_commit_cert_t,     TBFT_CERT_COMMIT_MSG_SIZE,     COMMIT_DIGEST_OFFSET,     TBFT_MSG_COMMIT)
CERT_IMPL(tbft_checkpoint_cert, tbft_checkpoint_cert_t, TBFT_CERT_CHECKPOINT_MSG_SIZE, CHECKPOINT_DIGEST_OFFSET, TBFT_MSG_CHECKPOINT)
CERT_IMPL(tbft_prepare_cert,    tbft_prepare_cert_t,    TBFT_CERT_PREPARE_MSG_SIZE,    PREPARE_DIGEST_OFFSET,    TBFT_MSG_PREPARE)
