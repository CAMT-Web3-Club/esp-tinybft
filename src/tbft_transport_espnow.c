/**
 * @file tbft_transport_espnow.c
 * @brief แบ็กเอนด์การส่งข้อมูลผ่าน ESP-NOW พร้อมระบบแบ่งส่วน/ประกอบกลับข้อมูลอัตโนมัติ (fragmentation/reassembly)
 */

#include "tbft_transport.h"
#include "tbft_message.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <limits.h>
#include <string.h>

static constexpr char TAG[] = "tbft_espnow";

#ifndef MACSTR
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

/* --------------------------------------------------------------------------
 * ค่าคงที่สำหรับการแบ่งส่วนข้อมูล (Fragmentation constants)
 * -------------------------------------------------------------------------- */

#ifndef ESPNOW_MAX_DATA_LEN
#define ESPNOW_MAX_DATA_LEN  ESP_NOW_MAX_DATA_LEN_V2  /* 1470 */
#endif

static constexpr int FRAG_HDR_SIZE    = 4;
static constexpr int FRAG_MAX_PAYLOAD = ESPNOW_MAX_DATA_LEN - FRAG_HDR_SIZE;
static constexpr int FRAG_MAX_PARTS   = (TBFT_MAX_MESSAGE_SIZE + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD;
static constexpr int REASM_MAX_SLOTS  = 8;
static constexpr int REASM_TIMEOUT_MS = 5'000;
static constexpr int ESPNOW_TX_MAX_ATTEMPTS  = CONFIG_TBFT_ESPNOW_TX_RETRIES;
static constexpr int ESPNOW_TX_CB_TIMEOUT_MS = 100;   /* wait for the MAC-layer result */

static constexpr int MSG_QUEUE_DEPTH      = 16;
static constexpr int SEND_QUEUE_DEPTH     = 8;
static constexpr int SEND_TASK_PRIORITY   = 7;
static constexpr int SEND_TASK_STACK_SIZE = 8'192;
static constexpr int SEND_TASK_RECV_TIMEOUT_MS = 1'000;  /* only to notice shutdown; keeps the CPU asleep */
static constexpr int SEND_QUEUE_TIMEOUT_MS     = 100;
static constexpr int SHUTDOWN_DRAIN_MS         = 200;

static constexpr int SEND_TASK_SHUTDOWN = -999;
static_assert(SEND_TASK_SHUTDOWN < 0, "SEND_TASK_SHUTDOWN must be negative");

static constexpr int ESPNOW_MAC_LEN = 6;

/* true when ESP-NOW runs on the softAP interface (TBFT_ESPNOW_IF_AP) */
#if CONFIG_TBFT_ESPNOW_IF_AP
static constexpr bool ESPNOW_IF_IS_AP = true;
#else
static constexpr bool ESPNOW_IF_IS_AP = false;
#endif

/* --------------------------------------------------------------------------
 * โครงสร้างข้อมูลคิวและการแบ่งส่วน (Structures)
 * -------------------------------------------------------------------------- */

typedef struct {
    int      buf_len;
    uint8_t  src_mac[ESPNOW_MAC_LEN];
    uint8_t  payload[TBFT_MAX_MESSAGE_SIZE];
} recv_entry_t;

typedef struct [[gnu::packed]] {
    uint16_t msg_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
} frag_hdr_t;

static_assert(sizeof(frag_hdr_t) == FRAG_HDR_SIZE, "frag_hdr_t size mismatch");
static_assert(FRAG_MAX_PARTS <= 32, "TBFT_MAX_MESSAGE_SIZE too large");
/* NOTE: When TBFT_MAX_MESSAGE_SIZE <= ESPNOW_MAX_DATA_LEN (1470), all messages
 * fit in a single fragment and the multi-fragment code path is dead code.
 * When TBFT_MAX_MESSAGE_SIZE > 1470, fragmentation is active and tested. */

typedef struct {
    uint8_t  buf[TBFT_MAX_MESSAGE_SIZE];
    int      len;
    int      dest;
} send_entry_t;

typedef struct {
    uint8_t   buf[TBFT_MAX_MESSAGE_SIZE];
    int       buf_len;
    uint16_t  msg_id;
    uint8_t   frag_total;
    uint8_t   frag_received;
    uint32_t  frag_mask;
    uint8_t   src_mac[6];
    bool      valid;
    bool      stale;
    TickType_t last_tick;
    uint16_t  last_frag_len;
} reasm_slot_t;

typedef struct tbft_espnow {
    tbft_addr_t  peers[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    bool         peer_valid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    uint8_t      peer_ap_mac[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS][6];
    int          num_nodes;
    int          num_replicas;
    int          local_id;

    QueueHandle_t msg_queue;
    QueueHandle_t send_queue;
    TaskHandle_t  send_task_handle;
    SemaphoreHandle_t task_exit_sem; /* ซิงก์รอให้ task ออกอย่างปลอดภัยตอน shutdown */

    reasm_slot_t  reasm[REASM_MAX_SLOTS];
    uint16_t next_msg_id;
    SemaphoreHandle_t lock;
    volatile bool shutting_down;

    /* TX completion: only send_task calls esp_now_send(), so at most one
     * frame is in flight; the send callback reports its MAC-layer result. */
    SemaphoreHandle_t tx_done;
    SemaphoreHandle_t rx_sem;   /* given on every delivered message (wait()) */
    volatile esp_now_send_status_t tx_status;
    uint8_t tx_expect_mac[6];

    tbft_transport_stats_t stats;
} tbft_espnow_t;

static tbft_espnow_t *g_espnow_ctx = nullptr;

/* --------------------------------------------------------------------------
 * ฟังก์ชันคำนวณ MAC Address อย่างปลอดภัย (รองรับ Overflow/Underflow)
 * -------------------------------------------------------------------------- */

#if CONFIG_TBFT_ESPNOW_IF_AP
static constexpr wifi_interface_t TBFT_ESPNOW_IFIDX = WIFI_IF_AP;
#else
static constexpr wifi_interface_t TBFT_ESPNOW_IFIDX = WIFI_IF_STA;
#endif

/* "AP MAC" = the on-air address of the interface ESP-NOW uses.  For the
 * softAP interface it is the base (eFuse) MAC + 1; for the station
 * interface it is the base MAC itself. */
static void get_ap_mac(const uint8_t *base_mac, uint8_t *ap_mac) {
    memcpy(ap_mac, base_mac, 6);
#if !CONFIG_TBFT_ESPNOW_IF_AP
    return;
#endif
    for (int i = 5; i >= 0; i--) {
        ap_mac[i]++;
        if (ap_mac[i] != 0) break; /* หากไม่เกิด Overflow (255 -> 0) ให้หยุดคำนวณทดบิต */
    }
}

static void get_base_mac(const uint8_t *ap_mac, uint8_t *base_mac) {
    memcpy(base_mac, ap_mac, 6);
#if !CONFIG_TBFT_ESPNOW_IF_AP
    return;
#endif
    for (int i = 5; i >= 0; i--) {
        uint8_t val = base_mac[i];
        base_mac[i]--;
        if (val != 0) break; /* หากไม่เกิด Underflow (0 -> 255) ให้หยุดคำนวณทดบิต */
    }
}

/* --------------------------------------------------------------------------
 * ฟังก์ชันจัดการ Reassembly (Stale-slot / Find / Feed)
 * -------------------------------------------------------------------------- */

static TickType_t now_ticks(void) {
    return xTaskGetTickCount();
}

static void reasm_clear_stale(tbft_espnow_t *enow) {
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (s->stale) {
            if (s->valid && s->frag_received < s->frag_total) {
                ESP_LOGD(TAG, "reasm: clearing stale slot %d "
                         "(msg_id=%u, %d/%d frags, %ums old)",
                         i, (unsigned)s->msg_id, s->frag_received,
                         s->frag_total,
                         (unsigned)((now_ticks() - s->last_tick) * portTICK_PERIOD_MS));
            }
            memset(s, 0, sizeof(*s));
            s->stale = false;
        }
    }
}

static reasm_slot_t *reasm_find_or_alloc(tbft_espnow_t *enow, uint16_t msg_id, const uint8_t *src_mac) {
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (s->valid && s->msg_id == msg_id && memcmp(s->src_mac, src_mac, 6) == 0) {
            return s;
        }
    }

    TickType_t now = now_ticks();
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (!s->valid) return s;
        TickType_t elapsed = now - s->last_tick;
        if (elapsed > pdMS_TO_TICKS(REASM_TIMEOUT_MS)) {
            /* Slot timed out — clear it immediately so the new message
             * starts with a clean buffer. */
            if (s->valid && s->frag_received > 0) {
                ESP_LOGW(TAG, "reasm: slot %d timed out (msg_id=%u, %d/%d frags, "
                         "%ums old) — clearing for new message",
                         i, (unsigned)s->msg_id, s->frag_received,
                         s->frag_total,
                         (unsigned)(elapsed * portTICK_PERIOD_MS));
            }
            memset(s, 0, sizeof(*s));
            s->last_tick = now;
            return s;
        }
    }

    /* All slots full and non-stale: evict the oldest. */
    int oldest = 0;
    TickType_t oldest_tick = enow->reasm[0].last_tick;
    for (int i = 1; i < REASM_MAX_SLOTS; i++) {
        if (enow->reasm[i].last_tick < oldest_tick) {
            oldest = i;
            oldest_tick = enow->reasm[i].last_tick;
        }
    }
    /* Log eviction of a partial message so operators can diagnose
     * high fragment loss or undersized REASM_MAX_SLOTS. */
    if (enow->reasm[oldest].valid && enow->reasm[oldest].frag_received > 0) {
        ESP_LOGW(TAG, "reasm: evicting slot %d (msg_id=%u, %d/%d frags) — "
                 "all slots full, oldest entry dropped",
                 oldest, (unsigned)enow->reasm[oldest].msg_id,
                 enow->reasm[oldest].frag_received,
                 enow->reasm[oldest].frag_total);
    }
    memset(&enow->reasm[oldest], 0, sizeof(reasm_slot_t));
    enow->reasm[oldest].last_tick = now;
    return &enow->reasm[oldest];
}

static bool reasm_feed(tbft_espnow_t *enow, reasm_slot_t *s, const uint8_t *data, int data_len, uint16_t msg_id, uint8_t frag_idx, uint8_t frag_total, const uint8_t *src_mac) {
    if (!s->valid || s->stale) {
        if (frag_total == 0 || frag_total > 32) return false;
        s->valid = true;
        s->stale = false;
        s->msg_id = msg_id;
        s->frag_total = frag_total;
        s->frag_received = 0;
        s->frag_mask = 0;
        s->buf_len = 0;
        s->last_frag_len = 0;
        memcpy(s->src_mac, src_mac, 6);
        s->last_tick = now_ticks();
    }

    if (msg_id != s->msg_id || frag_total != s->frag_total) {
        ESP_LOGW(TAG, "reasm: rejecting frag %d/%d — msg_id=%u mismatch "
                 "(expected %u, frag_total mismatch %d vs %d)",
                 frag_idx, frag_total, (unsigned)msg_id,
                 (unsigned)s->msg_id, frag_total, s->frag_total);
        return false;
    }
    if (frag_idx >= frag_total || frag_idx >= 32) {
        ESP_LOGW(TAG, "reasm: rejecting frag idx=%d (total=%d) — out of range",
                 frag_idx, frag_total);
        return false;
    }
    if (s->frag_mask & (1U << frag_idx)) {
        ESP_LOGD(TAG, "reasm: duplicate frag idx=%d for msg_id=%u, skipping",
                 frag_idx, (unsigned)s->msg_id);
        return false;
    }

    int offset = (int)frag_idx * FRAG_MAX_PAYLOAD;
    if (offset + data_len > TBFT_MAX_MESSAGE_SIZE) {
        ESP_LOGW(TAG, "reasm: rejecting frag idx=%d — would overflow buffer "
                 "(offset=%d + len=%d > %d)",
                 frag_idx, offset, data_len, TBFT_MAX_MESSAGE_SIZE);
        return false;
    }

    memcpy(s->buf + offset, data, (size_t)data_len);
    s->frag_mask |= (1U << frag_idx);
    s->frag_received++;
    s->last_tick = now_ticks();

    if (frag_idx == frag_total - 1) {
        s->last_frag_len = (uint16_t)data_len;
    }

    if (s->frag_received == frag_total) {
        s->buf_len = (int)(frag_total - 1) * FRAG_MAX_PAYLOAD + (int)s->last_frag_len;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Callback ฝั่ง รับ-ส่ง (WiFi Context)
 * -------------------------------------------------------------------------- */

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    tbft_espnow_t *enow = g_espnow_ctx;
    if (!enow || !tx_info) return;
    /* Ignore a late result belonging to an earlier (timed-out) frame. */
    if (memcmp(tx_info->des_addr, enow->tx_expect_mac, 6) != 0) return;
    enow->tx_status = status;
    xSemaphoreGive(enow->tx_done);   /* Wi-Fi task context, not an ISR */
}

static inline void stats_count_tag(uint32_t *arr, int tag) {
    if (tag > 0 && tag < TBFT_STATS_NUM_TAGS) arr[tag]++;
}

/* Hand one complete message to the protocol task. Wi-Fi task context. */
static void espnow_deliver(tbft_espnow_t *enow, recv_entry_t *q_entry) {
    int msg_tag = q_entry->payload[0]; /* low byte of hdr.tag */
    if (xQueueSend(enow->msg_queue, q_entry, 0) != pdTRUE) {
        enow->stats.rx_drop++;
        ESP_LOGD(TAG, "msg_queue full, dropping tag=%d len=%d", msg_tag, q_entry->buf_len);
        return;
    }
    enow->stats.rx_msgs++;
    stats_count_tag(enow->stats.rx_by_tag, msg_tag);
    xSemaphoreGive(enow->rx_sem);
}

/* Runs in the Wi-Fi task.  Keep it short: no logging, no lock, no large
 * stack objects.  The reassembly table is touched ONLY from here, so it
 * needs no mutex (the Wi-Fi task is the sole caller). */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len) {
    if (!g_espnow_ctx || !recv_info || !data) return;
    tbft_espnow_t *enow = g_espnow_ctx;
    if (data_len < (int)sizeof(frag_hdr_t)) { enow->stats.rx_frag_drop++; return; }

    /* Static: single caller (Wi-Fi task) -> avoids ~2 KB on its stack. */
    static recv_entry_t q_entry;

    const uint8_t *src_mac = recv_info->src_addr;
    const frag_hdr_t *fhdr = (const frag_hdr_t *)data;
    const uint8_t *payload = data + sizeof(frag_hdr_t);
    int payload_len = data_len - (int)sizeof(frag_hdr_t);

    /* Fast path: every message except New_key fits one frame. */
    if (fhdr->frag_total == 1) {
        if (fhdr->frag_idx != 0 || payload_len <= 0 || payload_len > TBFT_MAX_MESSAGE_SIZE) {
            enow->stats.rx_frag_drop++;
            return;
        }
        memcpy(q_entry.src_mac, src_mac, ESPNOW_MAC_LEN);
        q_entry.buf_len = payload_len;
        memcpy(q_entry.payload, payload, (size_t)payload_len);
        espnow_deliver(enow, &q_entry);
        return;
    }

    reasm_clear_stale(enow);
    reasm_slot_t *s = reasm_find_or_alloc(enow, fhdr->msg_id, src_mac);
    if (!s) { enow->stats.rx_frag_drop++; return; }

    if (!reasm_feed(enow, s, payload, payload_len, fhdr->msg_id,
                    fhdr->frag_idx, fhdr->frag_total, src_mac)) {
        return;   /* incomplete (or rejected fragment, logged at WARN inside) */
    }

    memcpy(q_entry.src_mac, src_mac, ESPNOW_MAC_LEN);
    q_entry.buf_len = s->buf_len;
    memcpy(q_entry.payload, s->buf, (size_t)s->buf_len);
    s->stale = true;
    espnow_deliver(enow, &q_entry);
}

/* --------------------------------------------------------------------------
 * ฟังก์ชันส่งข้อมูลและ Send Task
 * -------------------------------------------------------------------------- */

/* Send one frame and wait for its MAC-layer result (send callback).
 * esp_now_send() returning ESP_OK only means "queued"; the callback tells
 * whether the peer ACKed (ESP-NOW docs, "Send ESP-NOW Data").
 *
 * Retries on either kind of failure: at least ESPNOW_TX_MAX_ATTEMPTS times
 * and, with receive power saving, until TBFT_ESPNOW_RETRY_SPAN_MS has passed
 * (tbft_config.h).  The time bound is for receivers that duty-cycle their
 * radio: as long as the gap between attempts is shorter than their wake
 * window, an attempt lands in a window before the span (> their wake
 * interval) ends, however quickly the driver reports each failure. */
static bool espnow_tx_frame(tbft_espnow_t *enow, const uint8_t *mac,
                            const uint8_t *pkt, size_t len) {
#if TBFT_ESPNOW_RETRY_SPAN_MS > 0
    const int64_t give_up_us = esp_timer_get_time()
        + (int64_t)TBFT_ESPNOW_RETRY_SPAN_MS * 1000;
#endif
    for (int attempt = 0; ; attempt++) {
        memcpy(enow->tx_expect_mac, mac, 6);
        xSemaphoreTake(enow->tx_done, 0);          /* clear any stale result */
        esp_err_t err = esp_now_send(mac, pkt, len);
        if (err == ESP_OK) {
            enow->stats.tx_frames++;
            if (xSemaphoreTake(enow->tx_done,
                               pdMS_TO_TICKS(ESPNOW_TX_CB_TIMEOUT_MS)) == pdTRUE &&
                enow->tx_status == ESP_NOW_SEND_SUCCESS) {
                return true;
            }
        } else {
            ESP_LOGD(TAG, "esp_now_send err=0x%x (attempt %d)", err, attempt + 1);
        }
        bool again = (attempt + 1 < ESPNOW_TX_MAX_ATTEMPTS);
#if TBFT_ESPNOW_RETRY_SPAN_MS > 0
        again = again || (esp_timer_get_time() < give_up_us);
#endif
        if (!again) return false;
        vTaskDelay(TBFT_MS_TO_TICKS_MIN1(TBFT_ESPNOW_RETRY_DELAY_MS));
    }
}

static int espnow_do_send_mac(tbft_espnow_t *enow, const uint8_t *ap_mac, uint16_t msg_id, const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return -1;

    /* Stack packet buffer; send_task is the sole caller. */
    alignas(4) uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD];

    size_t remaining = len;
    const uint8_t *src = buf;
    uint8_t frag_total = (uint8_t)((len + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD);
    frag_hdr_t fhdr = { .msg_id = msg_id, .frag_total = frag_total };

    for (uint8_t frag_idx = 0; remaining > 0; frag_idx++) {
        fhdr.frag_idx = frag_idx;
        size_t chunk = remaining > FRAG_MAX_PAYLOAD ? FRAG_MAX_PAYLOAD : remaining;
        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), src, chunk);

        if (!espnow_tx_frame(enow, ap_mac, pkt, chunk + sizeof(fhdr))) {
            ESP_LOGW(TAG, "tx to " MACSTR " failed: frame %d/%d not acknowledged",
                     MAC2STR(ap_mac), frag_idx + 1, frag_total);
            return -1;   /* remaining frames are useless without this one */
        }
        src += chunk;
        remaining -= chunk;
    }
    return (int)len;
}

#if CONFIG_TBFT_ESPNOW_BROADCAST
static constexpr uint8_t k_bcast_mac[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
#endif

/* Apply TBFT_ESPNOW_PHY_RATE to a peer.  Must run after esp_wifi_start() and
 * esp_now_add_peer() (ESP-NOW docs).  0 keeps the IDF default (1 Mbps). */
static void espnow_apply_rate([[maybe_unused]] const uint8_t *mac) {
#if CONFIG_TBFT_ESPNOW_PHY_RATE != 0
    /* wifi_phy_rate_t value: 0x00-0x07 11b, 0x08-0x0F 11g, 0x10-0x17 HT20 */
    esp_now_rate_config_t rc = {
        .phymode = (CONFIG_TBFT_ESPNOW_PHY_RATE >= 0x10) ? WIFI_PHY_MODE_HT20
                 : (CONFIG_TBFT_ESPNOW_PHY_RATE >= 0x08) ? WIFI_PHY_MODE_11G
                 : WIFI_PHY_MODE_11B,
        .rate = (wifi_phy_rate_t)CONFIG_TBFT_ESPNOW_PHY_RATE,
    };
    esp_err_t rr = esp_now_set_peer_rate_config(mac, &rc);
    if (rr != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_set_peer_rate_config " MACSTR " failed: 0x%x",
                 MAC2STR(mac), rr);
    }
#endif
}

static int espnow_do_send(tbft_espnow_t *enow, tbft_node_id_t dest_id, uint16_t msg_id, const uint8_t *buf, size_t len) {
    if (dest_id < 0 || dest_id >= enow->num_nodes) return -1;
    return espnow_do_send_mac(enow, enow->peer_ap_mac[dest_id], msg_id, buf, len);
}

static void espnow_send_task(void *pvParameters) {
    tbft_espnow_t *enow = (tbft_espnow_t *)pvParameters;
    send_entry_t entry;

    while (1) {
        if (xQueueReceive(enow->send_queue, &entry, pdMS_TO_TICKS(SEND_TASK_RECV_TIMEOUT_MS)) != pdTRUE) {
            if (enow->shutting_down) {
                xSemaphoreGive(enow->task_exit_sem);
                vTaskDelete(nullptr);
            }
            continue;
        }

        if (entry.dest == SEND_TASK_SHUTDOWN) {
            ESP_LOGI(TAG, "send_task exiting cleanly");
            xSemaphoreGive(enow->task_exit_sem);
            vTaskDelete(nullptr);
        }

        if (entry.dest == TBFT_ALL_REPLICAS) {
            [[maybe_unused]] int limit = enow->num_replicas > 0 ? enow->num_replicas : enow->num_nodes;

            /* One msg_id per logical message, not per peer */
            uint16_t this_msg_id;
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

#if CONFIG_TBFT_ESPNOW_BROADCAST
            if (espnow_do_send_mac(enow, k_bcast_mac, this_msg_id, entry.buf, (size_t)entry.len) <= 0) {
                enow->stats.tx_fail++;
            }
            continue;
#endif
            for (int i = 0; i < limit; i++) {
                if (i == enow->local_id) continue;

                xSemaphoreTake(enow->lock, portMAX_DELAY);
                bool valid = enow->peer_valid[i];
                xSemaphoreGive(enow->lock);
                if (!valid) continue;

                if (espnow_do_send(enow, i, this_msg_id, entry.buf, (size_t)entry.len) <= 0) {
                    enow->stats.tx_fail++;
                }
            }
        } else if (entry.dest >= 0 && entry.dest < enow->num_nodes) {
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            bool valid = enow->peer_valid[entry.dest];
            uint16_t this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

            if (valid) {
                if (espnow_do_send(enow, entry.dest, this_msg_id, entry.buf, (size_t)entry.len) <= 0) {
                    enow->stats.tx_fail++;
                }
            } else {
                ESP_LOGW(TAG, "send: skipping unicast to node %d - peer not registered", entry.dest);
            }
        }
    }
}

static tbft_node_id_t find_node_by_mac(tbft_espnow_t *enow, const uint8_t *mac) {
    tbft_node_id_t result = -1;
    uint8_t base[6];
    get_base_mac(mac, base);
    xSemaphoreTake(enow->lock, portMAX_DELAY);
    for (int i = 0; i < enow->num_nodes; i++) {
        if (!enow->peer_valid[i]) continue;

        if (memcmp(enow->peers[i].u.mac.bytes, base, 6) == 0) {
            result = (tbft_node_id_t)i;
            break;
        }
    }
    xSemaphoreGive(enow->lock);
    return result;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int tbft_transport_create(tbft_transport_t **out, tbft_transport_type_t type, int num_nodes, int num_replicas, const char *mcast_ip, uint16_t port) {
    if (g_espnow_ctx != nullptr) {
        ESP_LOGE(TAG, "ESP-NOW transport is already initialized (Singleton Guard)");
        return -1;
    }

    if (num_nodes > TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) {
        ESP_LOGE(TAG, "num_nodes %d exceeds cap", num_nodes);
        return -1;
    }

    ESP_LOGI(TAG, "transport_create: free_heap=%u struct_size=%zu",
             (unsigned)esp_get_free_heap_size(), sizeof(tbft_espnow_t));

    tbft_espnow_t *enow = (tbft_espnow_t *)calloc(1, sizeof(*enow));
    if (!enow) {
        ESP_LOGE(TAG, "calloc(%zu) failed, free_heap=%u",
                 sizeof(tbft_espnow_t), (unsigned)esp_get_free_heap_size());
        return -1;
    }

    enow->num_nodes    = num_nodes;
    enow->num_replicas = num_replicas;
    enow->next_msg_id  = 1;

    enow->lock = xSemaphoreCreateMutex();
    enow->task_exit_sem = xSemaphoreCreateBinary();
    enow->tx_done = xSemaphoreCreateBinary();
    enow->rx_sem = xSemaphoreCreateBinary();
    enow->msg_queue = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(recv_entry_t));
    enow->send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_entry_t));

    if (!enow->lock || !enow->task_exit_sem || !enow->tx_done || !enow->rx_sem || !enow->msg_queue || !enow->send_queue) {
        ESP_LOGE(TAG, "queue/sem alloc failed: lock=%p sem=%p msg_q=%p send_q=%p free_heap=%u",
                 enow->lock, enow->task_exit_sem, enow->msg_queue, enow->send_queue,
                 (unsigned)esp_get_free_heap_size());
        if (enow->send_queue) vQueueDelete(enow->send_queue);
        if (enow->msg_queue) vQueueDelete(enow->msg_queue);
        if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
        if (enow->tx_done) vSemaphoreDelete(enow->tx_done);
        if (enow->rx_sem) vSemaphoreDelete(enow->rx_sem);
        if (enow->lock) vSemaphoreDelete(enow->lock);
        free(enow);
        return -1;
    }

    /* NOTE: xSemaphoreCreateMutex() creates the mutex in "not taken" state,
     * so an immediate xSemaphoreGive would return pdFALSE. Removed dead code. */
    g_espnow_ctx = enow;

    esp_err_t rcb_err = esp_now_register_recv_cb(espnow_recv_cb);
    if (rcb_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: 0x%x (%s) — "
                 "ensure esp_now_init() was called before Byz_init_replica",
                 rcb_err, esp_err_to_name(rcb_err));
        goto init_error;
    }
    esp_err_t scb_err = esp_now_register_send_cb(espnow_send_cb);
    if (scb_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_send_cb failed: 0x%x (%s)",
                 scb_err, esp_err_to_name(scb_err));
        esp_now_unregister_recv_cb();
        goto init_error;
    }

#if CONFIG_TBFT_ESPNOW_POWER_SAVE
    /* Receive power saving (Kconfig "Receive power saving"; ESP-IDF Wi-Fi
     * guide, "Connectionless Modules Power-saving"): RF on for WINDOW ms at
     * the start of every INTERVAL ms.  Same calls, in the same place (after
     * esp_now_init() and the callbacks), as ESP-IDF's examples/wifi/espnow.
     * WIFI_PS_MIN_MODEM is the driver default (esp_wifi.h); it is set again
     * in case the application chose WIFI_PS_NONE. */
    {
        esp_err_t pe = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (pe == ESP_OK) pe = esp_now_set_wake_window(CONFIG_TBFT_ESPNOW_WAKE_WINDOW_MS);
        if (pe == ESP_OK) pe = esp_wifi_connectionless_module_set_wake_interval(
                                   CONFIG_TBFT_ESPNOW_WAKE_INTERVAL_MS);
        if (pe != ESP_OK) {
            ESP_LOGE(TAG, "ESP-NOW receive power saving failed: 0x%x (%s)",
                     pe, esp_err_to_name(pe));
            goto init_error;
        }
    }
#endif

#if CONFIG_TBFT_ESPNOW_BROADCAST
    {
        esp_now_peer_info_t bp = { .channel = 0, .ifidx = TBFT_ESPNOW_IFIDX, .encrypt = false };
        memcpy(bp.peer_addr, k_bcast_mac, 6);
        esp_err_t be = esp_now_add_peer(&bp);
        if (be != ESP_OK && be != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(TAG, "add broadcast peer failed: 0x%x", be);
            esp_now_unregister_recv_cb();
            goto init_error;
        }
        espnow_apply_rate(k_bcast_mac);
    }
#endif
    if (xTaskCreate(espnow_send_task, "tbft_send", SEND_TASK_STACK_SIZE, enow, SEND_TASK_PRIORITY, &enow->send_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate for send_task failed");
        goto init_error;
    }

    *out = (tbft_transport_t *)enow;
    return 0;

init_error:
    g_espnow_ctx = nullptr;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    if (enow->send_queue) vQueueDelete(enow->send_queue);
    if (enow->msg_queue) vQueueDelete(enow->msg_queue);
    if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
    if (enow->tx_done) vSemaphoreDelete(enow->tx_done);
    if (enow->rx_sem) vSemaphoreDelete(enow->rx_sem);
    if (enow->lock) vSemaphoreDelete(enow->lock);
    free(enow);
    return -1;
}

void tbft_transport_free(tbft_transport_t *t) {
    if (!t) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    enow->shutting_down = true;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    g_espnow_ctx = nullptr;

    vTaskDelay(pdMS_TO_TICKS(50));

    if (enow->send_queue && enow->send_task_handle) {
        send_entry_t sentinel = { .dest = SEND_TASK_SHUTDOWN };
        if (xQueueSend(enow->send_queue, &sentinel, pdMS_TO_TICKS(SHUTDOWN_DRAIN_MS)) == pdTRUE) {
            if (xSemaphoreTake(enow->task_exit_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(TAG, "send_task did not exit cleanly, forcing delete");
                vTaskDelete(enow->send_task_handle);
            }
        } else {
            ESP_LOGW(TAG, "shutdown sentinel enqueue failed, forcing delete");
            vTaskDelete(enow->send_task_handle);
        }
    }

    if (enow->send_queue) vQueueDelete(enow->send_queue);
    if (enow->msg_queue) vQueueDelete(enow->msg_queue);
    if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
    if (enow->tx_done) vSemaphoreDelete(enow->tx_done);
    if (enow->rx_sem) vSemaphoreDelete(enow->rx_sem);
    if (enow->lock) vSemaphoreDelete(enow->lock);
    free(enow);
}

void tbft_transport_set_peer(tbft_transport_t *t, tbft_node_id_t node_id, const tbft_addr_t *addr) {
    if (!t || node_id < 0 || node_id >= TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    xSemaphoreTake(enow->lock, portMAX_DELAY);
    enow->peers[node_id] = *addr;
    enow->peer_valid[node_id] = true;

    /* Cache the peer's AP MAC for the hot send path */
    get_ap_mac(addr->u.mac.bytes, enow->peer_ap_mac[node_id]);

    uint8_t self_mac[6];
    if (esp_efuse_mac_get_default(self_mac) == ESP_OK && memcmp(self_mac, addr->u.mac.bytes, 6) == 0) {
        enow->local_id = node_id;
    }
    xSemaphoreGive(enow->lock);

    uint8_t ap_mac[6];
    get_ap_mac(addr->u.mac.bytes, ap_mac);

    ESP_LOGI(TAG, "set_peer node=%d base=" MACSTR " ap=" MACSTR,
             node_id, MAC2STR(addr->u.mac.bytes), MAC2STR(ap_mac));

    uint8_t self_base[6];
    uint8_t self_ap[6];
    if (esp_efuse_mac_get_default(self_base) == ESP_OK) {
        get_ap_mac(self_base, self_ap);
        if (ESPNOW_IF_IS_AP && memcmp(ap_mac, self_ap, 6) == 0) {
            ESP_LOGW(TAG, "set_peer: node %d AP MAC matches local AP MAC — "
                     "config may contain AP MAC instead of base (eFuse) MAC",
                     node_id);
        }
    }

    /* channel = 0: use current Wi-Fi channel (auto-detect).
     * Hardcoded channel 1 caused silent failures when AP was on a different
     * channel.  WiFi must be started before this call for channel 0 to
     * resolve correctly. */
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = TBFT_ESPNOW_IFIDX, .encrypt = false };
    memcpy(peer.peer_addr, ap_mac, 6);

    esp_now_del_peer(ap_mac);
    esp_err_t add_ret = esp_now_add_peer(&peer);
    if (add_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_add_peer for node %d failed: 0x%x", node_id, add_ret);
    }
    else {
        espnow_apply_rate(ap_mac);
    }
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len, tbft_node_id_t dest) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || !enow->send_queue || enow->shutting_down) {
        ESP_LOGD(TAG, "send rejected: transport %svalid, queue %svalid, shutting_down=%d",
                 enow ? "" : "in", enow && enow->send_queue ? "" : "in",
                 enow ? enow->shutting_down : -1);
        return -1;
    }
    if (len > (size_t)INT_MAX || len > TBFT_MAX_MESSAGE_SIZE) {
        ESP_LOGW(TAG, "send rejected: message too large (%zu bytes, max %d)",
                 len, TBFT_MAX_MESSAGE_SIZE);
        return -1;
    }

    send_entry_t entry;
    memcpy(entry.buf, buf, len);
    entry.len  = (int)len;
    entry.dest = dest;

    if (xQueueSend(enow->send_queue, &entry, pdMS_TO_TICKS(SEND_QUEUE_TIMEOUT_MS)) != pdTRUE) {
        enow->stats.tx_queue_full++;
        ESP_LOGW(TAG, "send_queue full, dropping msg to node %d (%d bytes)", dest, (int)len);
        return -1;
    }
    enow->stats.tx_msgs++;
    stats_count_tag(enow->stats.tx_by_tag, ((const uint8_t *)buf)[0]);
    return (int)len;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len, tbft_node_id_t *src_id) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || enow->shutting_down) return -1;

    /* M10 FIX: Use non-blocking recv (timeout=0) to match UDP transport
     * API contract. The previous 50ms blocking call capped the replica
     * loop at 20Hz, creating inconsistent behavior between transports. */
    recv_entry_t entry;
    if (xQueueReceive(enow->msg_queue, &entry, 0) != pdTRUE) return 0;

    if (entry.buf_len < 0 || (size_t)entry.buf_len > buf_len || (size_t)entry.buf_len > TBFT_MAX_MESSAGE_SIZE) return -1;

    memcpy(buf, entry.payload, (size_t)entry.buf_len);
    if (src_id) *src_id = find_node_by_mac(enow, entry.src_mac);

    return entry.buf_len;
}

void tbft_transport_get_stats(tbft_transport_t *t, tbft_transport_stats_t *out) {
    if (!t || !out) return;
    *out = ((tbft_espnow_t *)t)->stats;
}

void tbft_transport_wait(tbft_transport_t *t, int timeout_ms) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || enow->shutting_down) return;
    if (uxQueueMessagesWaiting(enow->msg_queue) > 0) return;
    xSemaphoreTake(enow->rx_sem, TBFT_MS_TO_TICKS_MIN1(timeout_ms));
}
