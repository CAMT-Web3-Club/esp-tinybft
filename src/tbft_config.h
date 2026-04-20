#pragma once

#include "sdkconfig.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Compile-time protocol parameters
 * These are sourced from Kconfig (sdkconfig.h) but can be overridden by
 * defining them before including this header.
 * -------------------------------------------------------------------------- */

#ifndef TBFT_MAX_MESSAGE_SIZE
#define TBFT_MAX_MESSAGE_SIZE    CONFIG_TBFT_MAX_MESSAGE_SIZE
#endif

#ifndef TBFT_MAX_REPLY_SIZE
#define TBFT_MAX_REPLY_SIZE      CONFIG_TBFT_MAX_REPLY_SIZE
#endif

#ifndef TBFT_BLOCK_SIZE
#define TBFT_BLOCK_SIZE          CONFIG_TBFT_BLOCK_SIZE
#endif

#ifndef TBFT_MAX_NUM_REPLICAS
#define TBFT_MAX_NUM_REPLICAS    CONFIG_TBFT_MAX_NUM_REPLICAS
#endif

#ifndef TBFT_WINDOW_SIZE
#define TBFT_WINDOW_SIZE         CONFIG_TBFT_WINDOW_SIZE
#endif

#ifndef TBFT_CHECKPOINT_INTERVAL
#define TBFT_CHECKPOINT_INTERVAL CONFIG_TBFT_CHECKPOINT_INTERVAL
#endif

#ifndef TBFT_MAX_NUM_CLIENTS
#define TBFT_MAX_NUM_CLIENTS     CONFIG_TBFT_MAX_NUM_CLIENTS
#endif

#ifndef TBFT_MAX_STATE_BLOCKS
#define TBFT_MAX_STATE_BLOCKS    CONFIG_TBFT_MAX_STATE_BLOCKS
#endif

#ifndef TBFT_RQUEUE_MAX
#define TBFT_RQUEUE_MAX          CONFIG_TBFT_RQUEUE_MAX
#endif

#ifndef TBFT_NDET_BUF_SIZE
#define TBFT_NDET_BUF_SIZE       CONFIG_TBFT_NDET_BUF_SIZE
#endif

#ifndef TBFT_P_LEVELS
#define TBFT_P_LEVELS            CONFIG_TBFT_P_LEVELS
#endif

#ifndef TBFT_ANTI_REPLAY_WINDOW_US
#define TBFT_ANTI_REPLAY_WINDOW_US  ((int64_t)CONFIG_TBFT_ANTI_REPLAY_WINDOW_MS * 1000LL)
#endif

#ifndef TBFT_VIEW_CHANGE_TIMEOUT_US
#define TBFT_VIEW_CHANGE_TIMEOUT_US  CONFIG_TBFT_VIEW_CHANGE_TIMEOUT_US
#endif

#ifndef TBFT_STATUS_TIMEOUT_US
#define TBFT_STATUS_TIMEOUT_US       CONFIG_TBFT_STATUS_TIMEOUT_US
#endif

/* --------------------------------------------------------------------------
 * Derived protocol constants
 * -------------------------------------------------------------------------- */

/* Cryptographic sizes */
#define TBFT_DIGEST_SIZE         32   /* SHA-256 output bytes */
#define TBFT_HMAC_SIZE           32   /* HMAC-SHA256 output bytes */
#define TBFT_HMAC_KEY_SIZE       32   /* Symmetric session key bytes */
#define TBFT_RSA_KEY_BITS        2048
#define TBFT_SIG_SIZE            (TBFT_RSA_KEY_BITS / 8)  /* 256 bytes */

/* Authenticator: one HMAC slot per remote replica */
#define TBFT_AUTH_SIZE           (TBFT_HMAC_SIZE * (TBFT_MAX_NUM_REPLICAS - 1))

/* Checkpoint log slots: window/interval + 2 for overlap */
#define TBFT_NUM_CKPT_SLOTS      (TBFT_WINDOW_SIZE / TBFT_CHECKPOINT_INTERVAL + 2)

/* Maximum f (Byzantine faults) for sizing — actual f set at runtime */
#define TBFT_MAX_FAULTY          ((TBFT_MAX_NUM_REPLICAS - 1) / 3)

/* Max distinct values stored in a certificate: f+1 */
#define TBFT_CERT_MAX_VALS       (TBFT_MAX_FAULTY + 1)

/* Partition tree: children per internal node */
#define TBFT_P_CHILDREN          \
    ((TBFT_MAX_MESSAGE_SIZE - 32) / (TBFT_DIGEST_SIZE + sizeof(int32_t)))

/* --------------------------------------------------------------------------
 * Static assertions (compile-time validation)
 * -------------------------------------------------------------------------- */

_Static_assert((TBFT_BLOCK_SIZE & (TBFT_BLOCK_SIZE - 1)) == 0,
               "TBFT_BLOCK_SIZE must be a power of two");

_Static_assert(TBFT_WINDOW_SIZE > TBFT_CHECKPOINT_INTERVAL,
               "TBFT_WINDOW_SIZE must be greater than TBFT_CHECKPOINT_INTERVAL");

_Static_assert((TBFT_WINDOW_SIZE & (TBFT_WINDOW_SIZE - 1)) == 0,
               "TBFT_WINDOW_SIZE must be a power of two");

_Static_assert(TBFT_MAX_NUM_REPLICAS >= 4,
               "Need at least 4 replicas for BFT (n = 3f+1, f >= 1)");

_Static_assert(TBFT_MAX_NUM_REPLICAS <= 64,
               "tbft_bitmap_t is uint64_t; bumping this above 64 silently drops "
               "sender accounting (tbft_bitmap_set/test become no-ops) and allows "
               "Byzantine re-voting to forge certificate quorums");

_Static_assert(TBFT_CHECKPOINT_INTERVAL >= 1,
               "TBFT_CHECKPOINT_INTERVAL must be >= 1 (division by zero in slot_index)");

_Static_assert(TBFT_MAX_REPLY_SIZE < TBFT_MAX_MESSAGE_SIZE,
               "TBFT_MAX_REPLY_SIZE must be less than TBFT_MAX_MESSAGE_SIZE");

_Static_assert(TBFT_P_LEVELS >= 2,
               "TBFT_P_LEVELS must be at least 2");
