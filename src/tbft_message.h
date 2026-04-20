#pragma once

#include "tbft_types.h"
#include <string.h>
#include <limits.h>

/* --------------------------------------------------------------------------
 * Message tag constants (section 4)
 * -------------------------------------------------------------------------- */

typedef enum {
    TBFT_MSG_REQUEST         = 1,
    TBFT_MSG_REPLY           = 2,
    TBFT_MSG_PRE_PREPARE     = 3,
    TBFT_MSG_PREPARE         = 4,
    TBFT_MSG_COMMIT          = 5,
    TBFT_MSG_CHECKPOINT      = 6,
    TBFT_MSG_STATUS          = 7,
    TBFT_MSG_VIEW_CHANGE     = 8,
    TBFT_MSG_NEW_VIEW        = 9,
    TBFT_MSG_VIEW_CHANGE_ACK = 10,
    TBFT_MSG_NEW_KEY         = 11,
    TBFT_MSG_META_DATA       = 12,
    TBFT_MSG_META_DATA_D     = 13,
    TBFT_MSG_DATA            = 14,
    TBFT_MSG_FETCH           = 15,
    TBFT_MSG_QUERY_STABLE    = 16,
    TBFT_MSG_REPLY_STABLE    = 17,
} tbft_msg_tag_t;

/* --------------------------------------------------------------------------
 * Common message header
 * All messages on the wire begin with this header (16 bytes).
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    int16_t  tag;   /* tbft_msg_tag_t */
    int16_t  extra; /* tag-specific flags */
    int32_t  size;  /* total message byte length (8-byte aligned) */
    int64_t  timestamp_us; /* sender timestamp for anti-replay protection */
} tbft_msg_hdr_t;

/* --------------------------------------------------------------------------
 * Request  (tag = 1)  Client → Primary
 * Wire: [hdr][Request_rep][command bytes][RSA signature]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_digest_t   od;           /* SHA-256 of command */
    int32_t         cid;          /* client id */
    tbft_req_id_t   rid;          /* request id */
    int32_t         replier;      /* -1 = all replicas reply */
    int32_t         command_size; /* bytes of command payload that follow */
} tbft_request_rep_t;

/* --------------------------------------------------------------------------
 * Reply  (tag = 2)  Replica → Client
 * Wire: [hdr][Reply_rep][reply payload][RSA signature]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     view;
    tbft_seqno_t    seqno;
    int32_t         cid;
    tbft_req_id_t   rid;
    int32_t         reply_size;   /* bytes of reply payload that follow */
} tbft_reply_rep_t;

/* --------------------------------------------------------------------------
 * Pre-prepare  (tag = 3)  Primary → Replicas
 * Wire: [hdr][Pre_prepare_rep][request_set bytes][non-det bytes][authenticator]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     view;
    tbft_seqno_t    seqno;
    tbft_digest_t   digest;       /* SHA-256 of request set */
    int32_t         rset_size;    /* bytes of request set */
    int16_t         non_det_size; /* bytes of non-deterministic choices */
    int16_t         _pad;
} tbft_pre_prepare_rep_t;

/* --------------------------------------------------------------------------
 * Prepare  (tag = 4)  Replica → All
 * Wire: [hdr][Prepare_rep][authenticator]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     view;
    tbft_seqno_t    seqno;
    tbft_digest_t   digest;
    int32_t         id;   /* sender's replica id */
    int32_t         _pad;
} tbft_prepare_rep_t;

/* --------------------------------------------------------------------------
 * Commit  (tag = 5)  Replica → All
 * Wire: [hdr][Commit_rep][authenticator]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     view;
    tbft_seqno_t    seqno;
    tbft_digest_t   digest;   /* SHA-256 digest of the request (PBFT: <COMMIT, v, n, d, i>) */
    int32_t         id;   /* sender's replica id */
    int32_t         _pad;
} tbft_commit_rep_t;

/* --------------------------------------------------------------------------
 * Checkpoint  (tag = 6)  Replica → All
 * Wire: [hdr][Checkpoint_rep][authenticator]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    seqno;
    tbft_digest_t   digest; /* state digest at this seqno */
    int32_t         id;     /* sender's replica id */
    int32_t         _pad;
} tbft_checkpoint_rep_t;

/* --------------------------------------------------------------------------
 * Status  (tag = 7)  Replica → All
 * Wire: [hdr][Status_rep][optional bitmaps]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     view;
    tbft_seqno_t    last_stable;
    tbft_seqno_t    last_prepared;
    tbft_seqno_t    last_executed;
    int32_t         id;
    int32_t         _pad;
} tbft_status_rep_t;

/* --------------------------------------------------------------------------
 * View_change  (tag = 8)  Replica → All
 * Wire: [hdr][View_change_rep][ckpts array][prepared bitmap][req_info array]
 *       [RSA signature]
 * -------------------------------------------------------------------------- */

/* One checkpoint entry embedded in a View_change */
typedef struct __attribute__((packed)) {
    tbft_seqno_t  seqno;
    tbft_digest_t digest;
} tbft_vc_ckpt_t;

/* One prepared request entry embedded in a View_change */
typedef struct __attribute__((packed)) {
    tbft_seqno_t  seqno;
    tbft_view_t   last_view;   /* view in which it was prepared */
    tbft_digest_t digest;
} tbft_vc_req_info_t;

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     v;        /* new view proposed */
    tbft_seqno_t    ls;       /* sender's last stable seqno */
    int32_t         n_ckpts;  /* number of tbft_vc_ckpt_t entries following */
    int32_t         n_reqs;   /* number of tbft_vc_req_info_t entries */
    int32_t         id;       /* sender's replica id */
    int32_t         _pad;
} tbft_view_change_rep_t;

/* --------------------------------------------------------------------------
 * New_view  (tag = 9)  New Primary → All
 * Wire: [hdr][New_view_rep][prepared array][Pre_prepare set][authenticator]
 *       [RSA signature of New_view_rep]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     v;        /* new view number */
    tbft_seqno_t    min;      /* max of all last_stable values in view-changes */
    tbft_seqno_t    max;      /* min of (last_stable + window) across view-changes */
    int32_t         n_prep;   /* number of prepared entries */
    int32_t         has_sig;  /* 1 if signature follows */
} tbft_new_view_rep_t;

/* --------------------------------------------------------------------------
 * View_change_ack  (tag = 10)  Replica → New Primary
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_view_t     v;
    int32_t         id;      /* sender id */
    int32_t         vc_id;   /* view-change message sender being ack'd */
} tbft_vc_ack_rep_t;

/* --------------------------------------------------------------------------
 * New_key  (tag = 11)  Replica → All
 * Wire: [hdr][New_key_rep][tbft_new_key_slot_t array, n_keys entries]
 *
 * Each slot carries one HMAC session key RSA-OAEP-encrypted for the named
 * recipient.  On receipt a node finds the slot with recipient_id == local_id,
 * decrypts it with its RSA private key, and stores the result as the HMAC
 * in-key for the sender.
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    int32_t         id;       /* sender's replica id */
    int32_t         n_keys;   /* number of tbft_new_key_slot_t entries */
} tbft_new_key_rep_t;

/** One encrypted-key slot inside a New_key message */
typedef struct __attribute__((packed)) {
    int32_t  recipient_id;
    uint8_t  ciphertext[TBFT_SIG_SIZE]; /* RSA-OAEP encrypted tbft_hmac_key_t */
} tbft_new_key_slot_t;

/* --------------------------------------------------------------------------
 * Meta_data  (tag = 12)  Replica → Fetching
 * Wire: [hdr][Meta_data_rep][Part_info array]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_digest_t digest;
    int32_t       version; /* checkpoint seqno version */
} tbft_part_info_t;

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    seqno;   /* checkpoint seqno being transferred */
    int32_t         level;   /* level in partition tree (0 = root) */
    int32_t         index;   /* node index at this level */
    int32_t         n_parts; /* number of tbft_part_info_t entries following */
    int32_t         _pad;
} tbft_meta_data_rep_t;

/* --------------------------------------------------------------------------
 * Meta_data_d  (tag = 13)  Replica → Fetching
 * Wire: [hdr][Meta_data_d_rep][digest array]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    seqno;
    int32_t         level;
    int32_t         index;
    int32_t         n_digests;
    int32_t         _pad;
} tbft_meta_data_d_rep_t;

/* --------------------------------------------------------------------------
 * Data  (tag = 14)  Replica → Fetching
 * Wire: [hdr][Data_rep][block_data bytes]
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    seqno;
    int32_t         block_index; /* index into state block array */
    int32_t         _pad;
    tbft_digest_t   digest;      /* expected digest of the block */
} tbft_data_rep_t;

/* --------------------------------------------------------------------------
 * Fetch  (tag = 15)  Fetching → Replier
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    last_stable;
    tbft_seqno_t    c;      /* checkpoint seqno being fetched */
    int32_t         level;
    int32_t         index;
    int32_t         id;     /* requesting replica id */
    int32_t         _pad;
} tbft_fetch_rep_t;

/* --------------------------------------------------------------------------
 * Query_stable  (tag = 16)  Replica → All
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_req_id_t   rid; /* request id for matching replies */
    int32_t         id;
    int32_t         _pad;
} tbft_query_stable_rep_t;

/* --------------------------------------------------------------------------
 * Reply_stable  (tag = 17)  Replica → Querier
 * -------------------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    tbft_msg_hdr_t  hdr;
    tbft_seqno_t    seqno;   /* max stable checkpoint known */
    tbft_req_id_t   rid;     /* matching query rid */
    int32_t         id;
    int32_t         _pad;
} tbft_reply_stable_rep_t;

/* --------------------------------------------------------------------------
 * Generic message wrapper (for in-memory manipulation)
 * -------------------------------------------------------------------------- */

/**
 * In-memory message: points into a pre-allocated buffer.
 * The buffer begins with a tbft_msg_hdr_t followed by the type-specific rep.
 */
typedef struct {
    uint8_t *buf;     /* pointer to raw message bytes */
    int      buf_cap; /* capacity of buf */
} tbft_msg_t;

/* --------------------------------------------------------------------------
 * Inline helpers
 * -------------------------------------------------------------------------- */

static inline tbft_msg_tag_t tbft_msg_tag(const tbft_msg_t *m)
{
    return (tbft_msg_tag_t)((const tbft_msg_hdr_t *)m->buf)->tag;
}

static inline int tbft_msg_size(const tbft_msg_t *m)
{
    return ((const tbft_msg_hdr_t *)m->buf)->size;
}

static inline void tbft_msg_set_hdr(tbft_msg_t *m, tbft_msg_tag_t tag,
                                    int16_t extra, int32_t size)
{
    tbft_msg_hdr_t *h = (tbft_msg_hdr_t *)m->buf;
    h->tag   = (int16_t)tag;
    h->extra = extra;
    h->size  = size;
}

/* Align size up to 8-byte boundary (as required by wire format).
 * Returns -1 if the size is negative or would overflow after alignment. */
static inline int32_t tbft_msg_align(int32_t size)
{
    if (size < 0) return -1;
    if (size > INT32_MAX - 7) return -1; /* overflow guard */
    return (size + 7) & ~7;
}

/* --------------------------------------------------------------------------
 * Function declarations
 * -------------------------------------------------------------------------- */

/**
 * Compute SHA-256 digest of data.
 * @param data    Input buffer
 * @param len     Input length in bytes
 * @param digest  Output digest
 */
void tbft_msg_digest(const void *data, size_t len, tbft_digest_t *digest);

/**
 * Compare two digests for equality.
 */
bool tbft_digest_equal(const tbft_digest_t *a, const tbft_digest_t *b);

/**
 * Zero a digest.
 */
static inline void tbft_digest_zero(tbft_digest_t *d)
{
    memset(d->bytes, 0, sizeof(d->bytes));
}
