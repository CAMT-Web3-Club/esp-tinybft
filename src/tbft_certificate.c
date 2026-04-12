#include "tbft_certificate.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Generic certificate logic (macro-expanded for each concrete type).
 *
 * All three certificate types (commit, checkpoint, prepare) follow identical
 * logic; only the slot size and type name differ.
 * -------------------------------------------------------------------------- */

#define CERT_IMPL(prefix, cert_t, msg_slot_size)                               \
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
    if (tbft_bitmap_test(&cert->bmap, sender_id)) {                            \
        return false; /* already have from this sender */                      \
    }                                                                          \
    /* Check if this message matches an existing value */                      \
    for (int i = 0; i < cert->num_vals; i++) {                                 \
        if (memcmp(cert->vals[i], msg,                                         \
                   (size_t)msg_len < (msg_slot_size) ?                         \
                   (size_t)msg_len : (msg_slot_size)) == 0) {                  \
            tbft_bitmap_set(&cert->bmap, sender_id);                           \
            cert->correct[i]++;                                                \
            return true;                                                       \
        }                                                                      \
    }                                                                          \
    /* New distinct value — only store up to f+1 */                            \
    if (cert->num_vals >= TBFT_CERT_MAX_VALS) {                                \
        return false; /* no room */                                            \
    }                                                                          \
    size_t copy_len = (size_t)msg_len < (msg_slot_size) ?                      \
                      (size_t)msg_len : (msg_slot_size);                       \
    memcpy(cert->vals[cert->num_vals], msg, copy_len);                         \
    cert->correct[cert->num_vals] = 1;                                         \
    tbft_bitmap_set(&cert->bmap, sender_id);                                   \
    cert->num_vals++;                                                          \
    return true;                                                               \
}                                                                              \
                                                                               \
bool prefix##_add_mine(cert_t *cert, const void *msg, int msg_len,             \
                       tbft_node_id_t my_id)                                   \
{                                                                              \
    if (tbft_bitmap_test(&cert->bmap, my_id)) {                                \
        return false;                                                          \
    }                                                                          \
    /* Store as first value if slot available */                               \
    if (cert->num_vals < TBFT_CERT_MAX_VALS) {                                 \
        size_t copy_len = (size_t)msg_len < (msg_slot_size) ?                  \
                          (size_t)msg_len : (msg_slot_size);                   \
        memcpy(cert->vals[cert->num_vals], msg, copy_len);                     \
        cert->correct[cert->num_vals] = 1;                                     \
        cert->mym_idx = cert->num_vals;                                        \
        cert->num_vals++;                                                      \
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

/* Expand for each type */
CERT_IMPL(tbft_commit_cert,     tbft_commit_cert_t,     TBFT_CERT_COMMIT_MSG_SIZE)
CERT_IMPL(tbft_checkpoint_cert, tbft_checkpoint_cert_t, TBFT_CERT_CHECKPOINT_MSG_SIZE)
CERT_IMPL(tbft_prepare_cert,    tbft_prepare_cert_t,    TBFT_CERT_PREPARE_MSG_SIZE)
