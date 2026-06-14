#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tbft_config.h"

/* --------------------------------------------------------------------------
 * Core protocol scalar types
 * -------------------------------------------------------------------------- */

/** Sequence number: monotonically increasing, globally ordered */
typedef int64_t  tbft_seqno_t;

/** View number: monotonically increasing view counter */
typedef int64_t  tbft_view_t;

/** Request identifier: (client_id << 48) | counter */
typedef uint64_t tbft_req_id_t;

/** Node identifier: index into principals array */
typedef int32_t  tbft_node_id_t;

/* Sentinel: send to all replicas */
#define TBFT_ALL_REPLICAS  (-1)

/* Sentinel: no valid sequence number */
#define TBFT_SEQNO_NONE    ((tbft_seqno_t)(-1LL))

/* --------------------------------------------------------------------------
 * Cryptographic types
 * -------------------------------------------------------------------------- */

/** SHA-256 digest (32 bytes) */
typedef struct {
    uint8_t bytes[TBFT_DIGEST_SIZE];
} tbft_digest_t;

/** HMAC-SHA256 message authentication code (32 bytes) */
typedef struct {
    uint8_t bytes[TBFT_HMAC_SIZE];
} tbft_mac_t;

/** Symmetric HMAC session key (32 bytes) */
typedef struct {
    uint8_t bytes[TBFT_HMAC_KEY_SIZE];
} tbft_hmac_key_t;

/** ECDSA P-256 signature (64 bytes: r(32) + s(32)) */
typedef struct {
    uint8_t bytes[TBFT_SIG_SIZE];
} tbft_sig_t;

/**
 * Authenticator: array of HMACs, one per remote replica.
 * Slot i holds the HMAC computed with the key shared with replica i.
 */
typedef struct {
    tbft_mac_t slots[TBFT_MAX_NUM_REPLICAS - 1];
} tbft_auth_t;

/* --------------------------------------------------------------------------
 * Network address
 * -------------------------------------------------------------------------- */

/** ESP-NOW MAC address (6 bytes) */
typedef struct {
    uint8_t bytes[6];
} tbft_mac_addr_t;

/**
 * Unified peer address — either an IPv4 + port pair, or an ESP-NOW MAC.
 * The active field depends on the configured transport type.
 */
typedef struct {
    union {
        struct {
            uint32_t ip;    /* network byte order */
            uint16_t port;  /* network byte order */
        } udp;
        tbft_mac_addr_t mac;
    } u;
} tbft_addr_t;

/* Convenience accessors */
#define tbft_addr_udp_ip(a)   ((a).u.udp.ip)
#define tbft_addr_udp_port(a) ((a).u.udp.port)
#define tbft_addr_mac_bytes(a) ((a).u.mac.bytes)

/* --------------------------------------------------------------------------
 * State block
 * -------------------------------------------------------------------------- */

/** Fixed-size state block (application data unit) */
typedef struct {
    uint8_t data[TBFT_BLOCK_SIZE];
} tbft_block_t;

/* --------------------------------------------------------------------------
 * Bitmap (up to 64 bits — supports MAX_NUM_REPLICAS <= 64)
 * -------------------------------------------------------------------------- */

typedef uint64_t tbft_bitmap_t;

static inline void tbft_bitmap_set(tbft_bitmap_t *bm, int i)
{
    if (i >= 0 && i < 64) *bm |= (1ULL << i);
}

static inline void tbft_bitmap_clear(tbft_bitmap_t *bm, int i)
{
    if (i >= 0 && i < 64) *bm &= ~(1ULL << i);
}

static inline bool tbft_bitmap_test(const tbft_bitmap_t *bm, int i)
{
    if (i < 0 || i >= 64) return false;
    return (*bm >> i) & 1ULL;
}

static inline void tbft_bitmap_zero(tbft_bitmap_t *bm)
{
    *bm = 0ULL;
}

static inline int tbft_bitmap_count(tbft_bitmap_t bm)
{
    /* popcount */
    int n = 0;
    while (bm) { n += (int)(bm & 1ULL); bm >>= 1; }
    return n;
}
