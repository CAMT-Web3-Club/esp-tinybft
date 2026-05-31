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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <limits.h>
#include <string.h>

static const char *TAG = "tbft_espnow";

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

#define FRAG_HDR_SIZE       4
#define FRAG_MAX_PAYLOAD    (ESPNOW_MAX_DATA_LEN - FRAG_HDR_SIZE)
#define FRAG_MAX_PARTS      ((TBFT_MAX_MESSAGE_SIZE + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD)
#define REASM_MAX_SLOTS     8
#define REASM_TIMEOUT_MS    5000
#define FRAG_INTER_DELAY_MS 20

#define MSG_QUEUE_DEPTH     16
#define SEND_QUEUE_DEPTH    8
#define SEND_TASK_PRIORITY  7
#define SEND_TASK_STACK_SIZE 8192
#define SEND_TASK_RECV_TIMEOUT_MS  50
#define RECV_TASK_TIMEOUT_MS  100
#define SEND_QUEUE_TIMEOUT_MS 100
#define SHUTDOWN_DRAIN_MS     200
/* ESP-NOW credits = peers per broadcast, capped at empirically-determined TX queue depth.
 * ESP-NOW driver is a precompiled blob; internal queue size is not documented.
 * The cap of 6 was determined by observation: >6 outstanding sends causes silent drops. */
#define ESPNOW_CREDITS_PER_CLUSTER  ((TBFT_MAX_NUM_REPLICAS - 1U))
#define ESPNOW_CREDITS_CAP          6
#define ESPNOW_TX_CREDITS           ((ESPNOW_CREDITS_PER_CLUSTER) < (ESPNOW_CREDITS_CAP) ? (ESPNOW_CREDITS_PER_CLUSTER) : (ESPNOW_CREDITS_CAP))
#define ESPNOW_CREDIT_TIMEOUT_MS    250

#define SEND_TASK_SHUTDOWN  -999
_Static_assert(SEND_TASK_SHUTDOWN < 0, "SEND_TASK_SHUTDOWN must be negative");

#define ESPNOW_MAC_LEN  6

/* --------------------------------------------------------------------------
 * โครงสร้างข้อมูลคิวและการแบ่งส่วน (Structures)
 * -------------------------------------------------------------------------- */

typedef struct {
    int      buf_len;
    uint8_t  src_mac[ESPNOW_MAC_LEN];
    uint8_t  payload[TBFT_MAX_MESSAGE_SIZE];
} recv_entry_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t msg_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
} frag_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(frag_hdr_t) == FRAG_HDR_SIZE, "frag_hdr_t size mismatch");
_Static_assert(FRAG_MAX_PARTS <= 32, "TBFT_MAX_MESSAGE_SIZE too large");
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
    volatile bool reasm_stale_flag;
    volatile bool shutting_down;
    SemaphoreHandle_t send_credit_sem;
} tbft_espnow_t;

static tbft_espnow_t *g_espnow_ctx = NULL;

/* --------------------------------------------------------------------------
 * ฟังก์ชันคำนวณ MAC Address อย่างปลอดภัย (รองรับ Overflow/Underflow)
 * -------------------------------------------------------------------------- */

static void get_ap_mac(const uint8_t *base_mac, uint8_t *ap_mac) {
    memcpy(ap_mac, base_mac, 6);
    for (int i = 5; i >= 0; i--) {
        ap_mac[i]++;
        if (ap_mac[i] != 0) break; /* หากไม่เกิด Overflow (255 -> 0) ให้หยุดคำนวณทดบิต */
    }
}

static void get_base_mac(const uint8_t *ap_mac, uint8_t *base_mac) {
    memcpy(base_mac, ap_mac, 6);
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
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "esp_now_send to " MACSTR " failed: %d", MAC2STR(tx_info->des_addr), (int)status);
    }
    if (g_espnow_ctx) {
        xSemaphoreGive(g_espnow_ctx->send_credit_sem);
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len) {
    if (!g_espnow_ctx || !recv_info || !data) return;
    if (data_len < (int)sizeof(frag_hdr_t)) return;

    tbft_espnow_t *enow = g_espnow_ctx;
    const uint8_t *src_mac = recv_info->src_addr;
    const frag_hdr_t *fhdr = (const frag_hdr_t *)data;

    /* M12 FIX: Log at DEBUG level to prevent massive log spam under load.
     * Every fragment arriving at high rate would overwhelm UART and increase
     * latency. Use DEBUG for per-fragment logging; completed messages
     * already log at INFO level below. */
    ESP_LOGD(TAG, "espnow_cb: frag %d/%d msg_id=%u len=%d from " MACSTR,
             fhdr->frag_idx, fhdr->frag_total, fhdr->msg_id, data_len, MAC2STR(src_mac));

    /* HIGH FIX H1: Keep lock timeout short (5ms) because recv_cb runs in
     * the WiFi task context. Espressif docs warn against lengthy operations
     * in WiFi callbacks. If the lock is held, drop the packet — BFT must
     * tolerate message loss anyway. */
    if (xSemaphoreTake(enow->lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    /* Proactively clear any stale reassembly slots before processing
     * a new fragment.  This prevents stale entries from consuming
     * slots when the send_task hasn't had a chance to run (e.g. under
     * heavy receive load where the queue never times out). */
    reasm_clear_stale(enow);
    enow->reasm_stale_flag = false;

    reasm_slot_t *s = reasm_find_or_alloc(enow, fhdr->msg_id, src_mac);
    if (!s) {
        xSemaphoreGive(enow->lock);
        return;
    }

    const uint8_t *payload = data + sizeof(frag_hdr_t);
    int payload_len = data_len - (int)sizeof(frag_hdr_t);

    bool complete = reasm_feed(enow, s, payload, payload_len, fhdr->msg_id, fhdr->frag_idx, fhdr->frag_total, src_mac);

    if (complete) {
        /* Copy completed message out of reassembly buffer BEFORE releasing lock,
         * so the slot can be reused by the next fragment while we queue. */
        recv_entry_t q_entry;
        memcpy(q_entry.src_mac, src_mac, ESPNOW_MAC_LEN);
        q_entry.buf_len = s->buf_len;
        memcpy(q_entry.payload, s->buf, (size_t)s->buf_len);
        int msg_tag = s->buf[0];

        s->stale = true;
        enow->reasm_stale_flag = true;

        /* Release lock BEFORE potentially blocking queue send to avoid
         * priority inversion with send_task. */
        xSemaphoreGive(enow->lock);

        if (xQueueSend(enow->msg_queue, &q_entry, 0) != pdTRUE) {
            if (enow->local_id >= enow->num_replicas && msg_tag == TBFT_MSG_REPLY) {
                ESP_LOGD(TAG, "client msg_queue full, dropping unsolicited reply len=%d",
                         q_entry.buf_len);
            } else {
                ESP_LOGW(TAG, "msg_queue full, dropping reassembled msg tag=%d len=%d",
                         msg_tag, q_entry.buf_len);
            }
        } else {
            ESP_LOGI(TAG, "recv: tag=%d len=%d from " MACSTR,
                     msg_tag, q_entry.buf_len, MAC2STR(src_mac));
        }
    } else {
        xSemaphoreGive(enow->lock);
    }
}

/* --------------------------------------------------------------------------
 * ฟังก์ชันส่งข้อมูลและ Send Task
 * -------------------------------------------------------------------------- */

static int espnow_do_send(tbft_espnow_t *enow, tbft_node_id_t dest_id, uint16_t msg_id, const uint8_t *buf, size_t len) {
    if (dest_id < 0 || dest_id >= enow->num_nodes || !buf || len == 0) return -1;

    const uint8_t *ap_mac = enow->peer_ap_mac[dest_id];

    /* CRITICAL FIX: Add per-fragment retry with exponential backoff.
     * Without this, a single ESP-NOW send failure causes complete message
     * loss, triggering unnecessary view-changes in the BFT protocol. */
    const int MAX_RETRIES = 3;
    const TickType_t credit_timeout = pdMS_TO_TICKS(ESPNOW_CREDIT_TIMEOUT_MS);

    /* Use stack buffer for packet assembly. Safe because send_task is
     * the sole caller and this function does not yield. */
    uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD] __attribute__((aligned(4)));

    if (len + sizeof(frag_hdr_t) <= ESPNOW_MAX_DATA_LEN) {
        frag_hdr_t fhdr = { .msg_id = msg_id, .frag_total = 1, .frag_idx = 0 };
        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), buf, len);

        int retries = 0;
        while (retries < MAX_RETRIES) {
            if (xSemaphoreTake(enow->send_credit_sem, credit_timeout) != pdTRUE) {
                ESP_LOGW(TAG, "espnow_do_send: no send credit after %dms, dropping",
                         ESPNOW_CREDIT_TIMEOUT_MS);
                return -1;
            }
            if (esp_now_send(ap_mac, pkt, len + sizeof(fhdr)) == ESP_OK) break;
            xSemaphoreGive(enow->send_credit_sem);
            retries++;
            if (retries < MAX_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(FRAG_INTER_DELAY_MS * retries));
            }
        }
        if (retries == MAX_RETRIES) {
            ESP_LOGW(TAG, "espnow_do_send: single-fragment failed after %d retries", MAX_RETRIES);
            return -1;
        }
        return (int)len;
    }

    size_t remaining = len;
    const uint8_t *src = buf;
    uint8_t frag_total = (uint8_t)((remaining + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD);
    uint8_t frag_idx = 0;

    frag_hdr_t fhdr = { .msg_id = msg_id, .frag_total = frag_total };

    while (remaining > 0) {
        fhdr.frag_idx = frag_idx;
        size_t chunk = remaining > FRAG_MAX_PAYLOAD ? FRAG_MAX_PAYLOAD : remaining;

        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), src, chunk);

        int retries = 0;
        while (retries < MAX_RETRIES) {
            if (xSemaphoreTake(enow->send_credit_sem, credit_timeout) != pdTRUE) {
                ESP_LOGW(TAG, "espnow_do_send: no send credit after %dms (frag %d/%d), dropping",
                         ESPNOW_CREDIT_TIMEOUT_MS,
                         frag_idx + 1, frag_total);
                return -1;
            }
            if (esp_now_send(ap_mac, pkt, chunk + sizeof(fhdr)) == ESP_OK) break;
            xSemaphoreGive(enow->send_credit_sem);
            retries++;
            if (retries < MAX_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(FRAG_INTER_DELAY_MS * retries));
            }
        }
        if (retries == MAX_RETRIES) {
            ESP_LOGW(TAG, "espnow_do_send: fragment %d/%d failed after %d retries",
                     frag_idx + 1, frag_total, MAX_RETRIES);
            return -1;
        }

        src += chunk;
        remaining -= chunk;
        frag_idx++;
    }
    return (int)len;
}

static void espnow_send_task(void *pvParameters) {
    tbft_espnow_t *enow = (tbft_espnow_t *)pvParameters;
    send_entry_t entry;

    while (1) {
        if (xQueueReceive(enow->send_queue, &entry, pdMS_TO_TICKS(SEND_TASK_RECV_TIMEOUT_MS)) != pdTRUE) {
            if (enow->shutting_down) {
                xSemaphoreGive(enow->task_exit_sem);
                vTaskDelete(NULL);
            }
            if (enow->reasm_stale_flag) {
                xSemaphoreTake(enow->lock, portMAX_DELAY);
                if (enow->reasm_stale_flag) {
                    enow->reasm_stale_flag = false;
                    reasm_clear_stale(enow);
                }
                xSemaphoreGive(enow->lock);
            }
            continue;
        }

        if (entry.dest == SEND_TASK_SHUTDOWN) {
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            reasm_clear_stale(enow);
            xSemaphoreGive(enow->lock);
            ESP_LOGI(TAG, "send_task exiting cleanly");

            /* ส่งสัญญาณให้กระบวนการ Free ถัดไปรับทราบว่า Task ปิดตัวเองเสร็จแล้ว */
            xSemaphoreGive(enow->task_exit_sem);
            vTaskDelete(NULL);
        }

        if (entry.dest == TBFT_ALL_REPLICAS) {
            int limit = enow->num_replicas > 0 ? enow->num_replicas : enow->num_nodes;
            int msg_tag = (entry.len > 0) ? entry.buf[0] : -1; /* first byte is hdr.tag */
            ESP_LOGI(TAG, "send_task: broadcast tag=%d size=%d to %d peers (limit=%d)",
                     msg_tag, entry.len, limit - 1, limit);

            /* Assign one msg_id per logical message, not per peer */
            uint16_t this_msg_id;
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

            for (int i = 0; i < limit; i++) {
                if (i == enow->local_id) continue;

                xSemaphoreTake(enow->lock, portMAX_DELAY);
                bool valid = enow->peer_valid[i];
                xSemaphoreGive(enow->lock);

                if (valid) {
                    int rc = espnow_do_send(enow, i, this_msg_id, entry.buf, (size_t)entry.len);
                    ESP_LOGI(TAG, "send_task: broadcast to node %d → %s (rc=%d)", i, rc > 0 ? "OK" : "FAIL", rc);
                } else {
                    ESP_LOGW(TAG, "send: skipping broadcast to node %d — peer not registered", i);
                }
            }
        } else if (entry.dest >= 0 && entry.dest < enow->num_nodes) {
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            bool valid = enow->peer_valid[entry.dest];
            uint16_t this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

            if (valid) {
                int msg_tag = entry.buf[0];
                int rc = espnow_do_send(enow, entry.dest, this_msg_id, entry.buf, (size_t)entry.len);
                ESP_LOGI(TAG, "send_task: unicast to node %d tag=%d → %s (rc=%d)",
                         entry.dest, msg_tag, rc > 0 ? "OK" : "FAIL", rc);
            } else {
                ESP_LOGW(TAG, "send: skipping unicast to node %d — peer not registered", entry.dest);
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
    if (g_espnow_ctx != NULL) {
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
    enow->msg_queue = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(recv_entry_t));
    enow->send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_entry_t));
    enow->send_credit_sem = xSemaphoreCreateCounting(ESPNOW_TX_CREDITS, ESPNOW_TX_CREDITS);

    if (!enow->lock || !enow->task_exit_sem || !enow->msg_queue || !enow->send_queue || !enow->send_credit_sem) {
        ESP_LOGE(TAG, "queue/sem alloc failed: lock=%p sem=%p msg_q=%p send_q=%p free_heap=%u",
                 enow->lock, enow->task_exit_sem, enow->msg_queue, enow->send_queue,
                 (unsigned)esp_get_free_heap_size());
        if (enow->send_credit_sem) vSemaphoreDelete(enow->send_credit_sem);
        if (enow->send_queue) vQueueDelete(enow->send_queue);
        if (enow->msg_queue) vQueueDelete(enow->msg_queue);
        if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
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

    if (xTaskCreate(espnow_send_task, "tbft_send", SEND_TASK_STACK_SIZE, enow, SEND_TASK_PRIORITY, &enow->send_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate for send_task failed");
        goto init_error;
    }

    *out = (tbft_transport_t *)enow;
    return 0;

init_error:
    g_espnow_ctx = NULL;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    if (enow->send_credit_sem) vSemaphoreDelete(enow->send_credit_sem);
    if (enow->send_queue) vQueueDelete(enow->send_queue);
    if (enow->msg_queue) vQueueDelete(enow->msg_queue);
    if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
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
    g_espnow_ctx = NULL;

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
    if (enow->send_credit_sem) vSemaphoreDelete(enow->send_credit_sem);
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
        if (memcmp(ap_mac, self_ap, 6) == 0) {
            ESP_LOGW(TAG, "set_peer: node %d AP MAC matches local AP MAC — "
                     "config may contain AP MAC instead of base (eFuse) MAC",
                     node_id);
        }
    }

    /* channel = 0: use current Wi-Fi channel (auto-detect).
     * Hardcoded channel 1 caused silent failures when AP was on a different
     * channel.  WiFi must be started before this call for channel 0 to
     * resolve correctly. */
    esp_now_peer_info_t peer = { .channel = 0, .ifidx = WIFI_IF_AP, .encrypt = false };
    memcpy(peer.peer_addr, ap_mac, 6);

    esp_now_del_peer(ap_mac);
    esp_err_t add_ret = esp_now_add_peer(&peer);
    if (add_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_add_peer for node %d failed: 0x%x", node_id, add_ret);
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
        ESP_LOGW(TAG, "send_queue full, dropping msg to node %d (%d bytes)", dest, (int)len);
        return -1;
    }
    return (int)len;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len, tbft_node_id_t *src_id) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || enow->shutting_down) return -1;

    if (enow->reasm_stale_flag) {
        xSemaphoreTake(enow->lock, portMAX_DELAY);
        if (enow->reasm_stale_flag) {
            enow->reasm_stale_flag = false;
            reasm_clear_stale(enow);
        }
        xSemaphoreGive(enow->lock);
    }

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
