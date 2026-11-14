#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"

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

#ifndef TBFT_RECOVERY_TIMEOUT_US
#ifdef CONFIG_TBFT_RECOVERY_TIMEOUT_US
#define TBFT_RECOVERY_TIMEOUT_US     CONFIG_TBFT_RECOVERY_TIMEOUT_US
#else
#define TBFT_RECOVERY_TIMEOUT_US     2500000LL
#endif
#endif

#ifndef TBFT_CLIENT_REPLY_TIMEOUT_MS
#define TBFT_CLIENT_REPLY_TIMEOUT_MS  CONFIG_TBFT_CLIENT_REPLY_TIMEOUT_MS
#endif

#ifndef TBFT_IDLE_WAIT_MS
#define TBFT_IDLE_WAIT_MS            CONFIG_TBFT_IDLE_WAIT_MS
#endif

/* ms -> ticks, never 0: at CONFIG_FREERTOS_HZ=100 pdMS_TO_TICKS(x<10) is 0,
 * which turns vTaskDelay() into a busy-wait. */
#define TBFT_MS_TO_TICKS_MIN1(ms) \
    ((pdMS_TO_TICKS(ms) > 0) ? pdMS_TO_TICKS(ms) : (TickType_t)1)

/* ESP-NOW retry timing (Kconfig "ESP-NOW radio").  Also printed in the
 * counter example's BANNER, so it is defined once, here.
 *
 * Radio always on: TBFT_ESPNOW_TX_RETRY_DELAY_MS between attempts and no time
 * floor.  Receive power saving: peers listen WINDOW ms at the start of every
 * INTERVAL ms, so a frame is repeated every half window until it is
 * acknowledged or INTERVAL + 20 ms have passed; with attempts closer than the
 * window, one of them falls inside a window.  Delays are whole ticks (at
 * least one), the unit vTaskDelay() works in. */
#if CONFIG_TBFT_TRANSPORT_ESPNOW
#define TBFT_TICK_MS  (1000 / CONFIG_FREERTOS_HZ)
#define TBFT_WHOLE_TICKS_MS(ms) \
    ((ms) >= TBFT_TICK_MS ? (ms) / TBFT_TICK_MS * TBFT_TICK_MS : TBFT_TICK_MS)
#if CONFIG_TBFT_ESPNOW_POWER_SAVE
#define TBFT_ESPNOW_RETRY_DELAY_MS  TBFT_WHOLE_TICKS_MS(CONFIG_TBFT_ESPNOW_WAKE_WINDOW_MS / 2)
#define TBFT_ESPNOW_RETRY_SPAN_MS   (CONFIG_TBFT_ESPNOW_WAKE_INTERVAL_MS + 20)
static_assert(CONFIG_TBFT_ESPNOW_WAKE_WINDOW_MS < CONFIG_TBFT_ESPNOW_WAKE_INTERVAL_MS,
              "ESP-NOW wake window must be shorter than the wake interval");
static_assert(TBFT_ESPNOW_RETRY_DELAY_MS < CONFIG_TBFT_ESPNOW_WAKE_WINDOW_MS,
              "ESP-NOW wake window too short for the FreeRTOS tick: a retry could skip it");
#else
#define TBFT_ESPNOW_RETRY_DELAY_MS  TBFT_WHOLE_TICKS_MS(CONFIG_TBFT_ESPNOW_TX_RETRY_DELAY_MS)
#define TBFT_ESPNOW_RETRY_SPAN_MS   0
#endif
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
    ((TBFT_MAX_MESSAGE_SIZE - 40) / (TBFT_DIGEST_SIZE + sizeof(int32_t)))

/* --------------------------------------------------------------------------
 * Static assertions (compile-time validation)
 * -------------------------------------------------------------------------- */

static_assert(TBFT_BLOCK_SIZE > 0 &&
              (TBFT_BLOCK_SIZE & (TBFT_BLOCK_SIZE - 1)) == 0,
              "TBFT_BLOCK_SIZE must be a positive power of two");

static_assert(TBFT_WINDOW_SIZE > TBFT_CHECKPOINT_INTERVAL,
              "TBFT_WINDOW_SIZE must be greater than TBFT_CHECKPOINT_INTERVAL");

static_assert(TBFT_WINDOW_SIZE > 0 &&
              (TBFT_WINDOW_SIZE & (TBFT_WINDOW_SIZE - 1)) == 0,
              "TBFT_WINDOW_SIZE must be a positive power of two");

static_assert(TBFT_MAX_NUM_REPLICAS >= 4,
              "Need at least 4 replicas for BFT (n = 3f+1, f >= 1)");

static_assert(TBFT_MAX_NUM_REPLICAS <= 64,
              "tbft_bitmap_t is uint64_t; bumping this above 64 silently drops "
              "sender accounting (tbft_bitmap_set/test become no-ops) and allows "
              "Byzantine re-voting to forge certificate quorums");

static_assert(TBFT_CHECKPOINT_INTERVAL >= 1,
              "TBFT_CHECKPOINT_INTERVAL must be >= 1 (division by zero in slot_index)");

static_assert(TBFT_MAX_REPLY_SIZE < TBFT_MAX_MESSAGE_SIZE,
              "TBFT_MAX_REPLY_SIZE must be less than TBFT_MAX_MESSAGE_SIZE");

static_assert(TBFT_MAX_MESSAGE_SIZE % 8 == 0,
              "TBFT_MAX_MESSAGE_SIZE must be a multiple of 8");

static_assert(TBFT_P_CHILDREN > 0,
              "TBFT_MAX_MESSAGE_SIZE too small for partition tree");

static_assert(TBFT_P_LEVELS >= 2,
              "TBFT_P_LEVELS must be at least 2");
