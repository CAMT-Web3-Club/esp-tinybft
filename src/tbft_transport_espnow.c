/**
 * @file tbft_transport_espnow.c
 * @brief ESP-NOW transport backend with automatic fragmentation/reassembly.
 *
 * Event-driven design:
 *  - Receive: ISR callback feeds reassembly, pushes to msg_queue, and calls
 *    xTaskNotifyFromISR() to wake the blocked replica task immediately.
 *  - Send: Replica task posts to send_queue (non-blocking).  A dedicated
 *    low-priority send_task drains the queue and handles ESP-NOW I/O with
 *    proper yields between sends.
 *
 * Fragment header (4 bytes, prepended to each fragment):
 *   uint16_t msg_id    — unique message identifier
 *   uint8_t  frag_idx  — fragment index (0-based)
 *   uint8_t  frag_total — total number of fragments
 */

#include "tbft_transport.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "tbft_espnow";

#ifndef MACSTR
#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#endif
#ifndef MAC2STR
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#endif

/* --------------------------------------------------------------------------
 * Fragmentation constants
 * -------------------------------------------------------------------------- */

#ifndef ESPNOW_MAX_DATA_LEN
#define ESPNOW_MAX_DATA_LEN  ESP_NOW_MAX_DATA_LEN_V2  /* 1470 */
#endif

#define FRAG_HDR_SIZE       4
#define FRAG_MAX_PAYLOAD    (ESPNOW_MAX_DATA_LEN - FRAG_HDR_SIZE)
#define FRAG_MAX_PARTS      ((TBFT_MAX_MESSAGE_SIZE + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD + 1)
#define REASM_MAX_SLOTS     4
#define REASM_TIMEOUT_MS    5000

#define SEND_QUEUE_DEPTH    8
#define SEND_TASK_PRIORITY  3   /* below replica task (typically 5) */

#pragma pack(push, 1)
typedef struct {
    uint16_t msg_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
} frag_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(frag_hdr_t) == FRAG_HDR_SIZE, "frag_hdr_t size mismatch");
_Static_assert(FRAG_MAX_PARTS <= 32,
    "TBFT_MAX_MESSAGE_SIZE too large: would need >32 ESP-NOW fragments");

/* --------------------------------------------------------------------------
 * Send queue entry
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  buf[TBFT_MAX_MESSAGE_SIZE];
    int      len;
    int      dest;   /* tbft_node_id_t (or TBFT_ALL_REPLICAS = -1) */
} send_entry_t;

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t   buf[TBFT_MAX_MESSAGE_SIZE];
    int       buf_len;
    uint16_t  msg_id;
    uint8_t   frag_total;
    uint8_t   frag_received;
    uint32_t  frag_mask;
    uint8_t   src_mac[6];
    bool      valid;
    TickType_t last_tick;
} reasm_slot_t;

typedef struct tbft_espnow {
    tbft_addr_t  peers[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    bool         peer_valid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    int          num_nodes;

    /* Queue of fully reassembled messages ready for delivery */
    QueueHandle_t msg_queue;

    /* Send queue: entries posted by replica task, consumed by send_task */
    QueueHandle_t send_queue;
    TaskHandle_t  send_task_handle;

    /* Reassembly slots */
    reasm_slot_t  reasm[REASM_MAX_SLOTS];

    /* Monotonic message ID counter (protected by lock) */
    uint16_t next_msg_id;

    /* Mutex for reassembly access */
    SemaphoreHandle_t lock;
} tbft_espnow_t;

static tbft_espnow_t *g_espnow_ctx = NULL;

/* --------------------------------------------------------------------------
 * Reassembly helpers
 * -------------------------------------------------------------------------- */

static TickType_t now_ticks(void)
{
    return xTaskGetTickCount();
}

static reasm_slot_t *reasm_find_or_alloc(tbft_espnow_t *enow,
                                          uint16_t msg_id,
                                          const uint8_t *src_mac)
{
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (s->valid && s->msg_id == msg_id
                && memcmp(s->src_mac, src_mac, 6) == 0) {
            return s;
        }
    }

    TickType_t now = now_ticks();
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (!s->valid) return s;
        TickType_t elapsed = now - s->last_tick;
        if (elapsed > pdMS_TO_TICKS(REASM_TIMEOUT_MS)) {
            memset(s, 0, sizeof(*s));
            return s;
        }
    }

    int oldest = 0;
    TickType_t oldest_tick = enow->reasm[0].last_tick;
    for (int i = 1; i < REASM_MAX_SLOTS; i++) {
        if (enow->reasm[i].last_tick < oldest_tick) {
            oldest = i;
            oldest_tick = enow->reasm[i].last_tick;
        }
    }
    memset(&enow->reasm[oldest], 0, sizeof(reasm_slot_t));
    return &enow->reasm[oldest];
}

static bool reasm_feed(reasm_slot_t *s, const uint8_t *data, int data_len,
                       uint16_t msg_id, uint8_t frag_idx, uint8_t frag_total,
                       const uint8_t *src_mac)
{
    if (!s->valid) {
        s->valid = true;
        s->msg_id = msg_id;
        s->frag_total = frag_total;
        s->frag_received = 0;
        s->frag_mask = 0;
        s->buf_len = 0;
        memcpy(s->src_mac, src_mac, 6);
        s->last_tick = now_ticks();
    }

    if (msg_id != s->msg_id || frag_total != s->frag_total) return false;
    if (frag_idx >= frag_total || frag_idx >= 32) return false;
    if (s->frag_mask & (1U << frag_idx)) return false;

    int offset = (int)frag_idx * FRAG_MAX_PAYLOAD;
    if (offset + data_len > TBFT_MAX_MESSAGE_SIZE) return false;

    memcpy(s->buf + offset, data, (size_t)data_len);
    s->frag_mask |= (1U << frag_idx);
    s->frag_received++;
    s->last_tick = now_ticks();

    if (s->frag_received == frag_total) {
        s->buf_len = offset + data_len;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * ESP-NOW receive callback (ISR context)
 * -------------------------------------------------------------------------- */

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (!g_espnow_ctx || !data || data_len < (int)sizeof(frag_hdr_t)) return;

    const uint8_t *src_mac = recv_info->src_addr;
    const frag_hdr_t *fhdr = (const frag_hdr_t *)data;

    tbft_espnow_t *enow = g_espnow_ctx;
    if (xSemaphoreTakeFromISR(enow->lock, NULL) != pdTRUE) return;

    reasm_slot_t *s = reasm_find_or_alloc(enow, fhdr->msg_id, src_mac);
    if (!s) { xSemaphoreGiveFromISR(enow->lock, NULL); return; }

    const uint8_t *payload = data + sizeof(frag_hdr_t);
    int payload_len = data_len - (int)sizeof(frag_hdr_t);

    bool complete = reasm_feed(s, payload, payload_len,
                               fhdr->msg_id, fhdr->frag_idx,
                               fhdr->frag_total, src_mac);

    if (complete) {
        uint8_t q_entry[6 + 4 + TBFT_MAX_MESSAGE_SIZE];
        memcpy(q_entry, src_mac, 6);
        memcpy(q_entry + 6, &s->buf_len, 4);
        memcpy(q_entry + 10, s->buf, (size_t)s->buf_len);

        if (xQueueSendFromISR(enow->msg_queue, q_entry, NULL) != pdTRUE) {
            /* Queue full — no warning in ISR to avoid latency */
        }
        memset(s, 0, sizeof(*s));

        /* Wake the blocked replica task immediately */
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (enow->send_task_handle) {
            vTaskNotifyGiveFromISR(enow->send_task_handle,
                                   &xHigherPriorityTaskWoken);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }

    xSemaphoreGiveFromISR(enow->lock, NULL);
}

/* --------------------------------------------------------------------------
 * Internal send function (called from send_task context)
 * -------------------------------------------------------------------------- */

static int espnow_do_send(tbft_espnow_t *enow,
                          const uint8_t *peer_mac, uint16_t msg_id,
                          const uint8_t *buf, size_t len)
{
    if (len + sizeof(frag_hdr_t) <= ESPNOW_MAX_DATA_LEN) {
        /* Single fragment */
        frag_hdr_t fhdr;
        fhdr.msg_id     = msg_id;
        fhdr.frag_total = 1;
        fhdr.frag_idx   = 0;

        uint8_t pkt[sizeof(frag_hdr_t) + ESPNOW_MAX_DATA_LEN];
        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), buf, len);

        esp_err_t err = esp_now_send(peer_mac, pkt, len + sizeof(fhdr));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send failed: %d", err);
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        return (int)len;
    }

    /* Multi-fragment */
    size_t remaining = len;
    const uint8_t *src = buf;
    uint8_t frag_total = (uint8_t)((remaining + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD);
    uint8_t frag_idx = 0;

    frag_hdr_t fhdr;
    fhdr.msg_id = msg_id;
    fhdr.frag_total = frag_total;

    uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD];

    while (remaining > 0) {
        fhdr.frag_idx = frag_idx;
        size_t chunk = remaining > FRAG_MAX_PAYLOAD ? FRAG_MAX_PAYLOAD : remaining;

        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), src, chunk);

        esp_err_t err = esp_now_send(peer_mac, pkt, chunk + sizeof(fhdr));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send frag %d/%d failed: %d",
                     frag_idx, frag_total, err);
            return -1;
        }

        vTaskDelay(pdMS_TO_TICKS(5));

        src += chunk;
        remaining -= chunk;
        frag_idx++;
    }
    return (int)len;
}

/* --------------------------------------------------------------------------
 * Send task — drains send_queue and handles ESP-NOW I/O
 * -------------------------------------------------------------------------- */

static void espnow_send_task(void *pvParameters)
{
    tbft_espnow_t *enow = (tbft_espnow_t *)pvParameters;
    send_entry_t entry;

    while (1) {
        /* Block until a send entry is available or a message is received */
        BaseType_t ret = xQueueReceive(enow->send_queue, &entry,
                                       pdMS_TO_TICKS(100));
        if (ret != pdTRUE) {
            /* Timeout — also check for task notification (from recv ISR) */
            if (ulTaskNotifyTake(pdFALSE, 0) == 0) {
                continue;
            }
        }

        if (entry.dest == TBFT_ALL_REPLICAS) {
            /* Broadcast: send to all registered peers */
            for (int i = 0; i < enow->num_nodes; i++) {
                if (!enow->peer_valid[i]) continue;

                uint16_t this_msg_id;
                xSemaphoreTake(enow->lock, portMAX_DELAY);
                this_msg_id = enow->next_msg_id++;
                xSemaphoreGive(enow->lock);

                espnow_do_send(enow, enow->peers[i].u.mac.bytes,
                               this_msg_id, entry.buf, (size_t)entry.len);
            }
        } else if (entry.dest >= 0 && entry.dest < enow->num_nodes
                   && enow->peer_valid[entry.dest]) {
            /* Unicast */
            uint16_t this_msg_id;
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

            espnow_do_send(enow, enow->peers[entry.dest].u.mac.bytes,
                           this_msg_id, entry.buf, (size_t)entry.len);
        } else {
            ESP_LOGE(TAG, "send_task: invalid dest %d", entry.dest);
        }
    }
}

/* --------------------------------------------------------------------------
 * Find node index from MAC address
 * -------------------------------------------------------------------------- */

static tbft_node_id_t find_node_by_mac(tbft_espnow_t *enow, const uint8_t *mac)
{
    for (int i = 0; i < enow->num_nodes; i++) {
        if (!enow->peer_valid[i]) continue;
        if (memcmp(enow->peers[i].u.mac.bytes, mac, 6) == 0) {
            return (tbft_node_id_t)i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int tbft_transport_create(tbft_transport_t **out,
                          tbft_transport_type_t type,
                          int num_nodes,
                          const char *mcast_ip,
                          uint16_t port)
{
    (void)type;
    (void)mcast_ip;
    (void)port;

    if (num_nodes > TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) {
        ESP_LOGE(TAG, "num_nodes %d exceeds max %d", num_nodes,
                 TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS);
        return -1;
    }

    tbft_espnow_t *enow = (tbft_espnow_t *)calloc(1, sizeof(*enow));
    if (!enow) return -1;

    enow->num_nodes = num_nodes;
    enow->next_msg_id = 1;

    enow->lock = xSemaphoreCreateMutex();
    if (!enow->lock) { free(enow); return -1; }

    enow->msg_queue = xQueueCreate(8, 6 + 4 + TBFT_MAX_MESSAGE_SIZE);
    if (!enow->msg_queue) {
        vSemaphoreDelete(enow->lock);
        free(enow);
        return -1;
    }

    enow->send_queue = xQueueCreate(SEND_QUEUE_DEPTH, sizeof(send_entry_t));
    if (!enow->send_queue) {
        vQueueDelete(enow->msg_queue);
        vSemaphoreDelete(enow->lock);
        free(enow);
        return -1;
    }

    g_espnow_ctx = enow;
    esp_now_register_recv_cb(espnow_recv_cb);

    /* Start the dedicated send task */
    BaseType_t ret = xTaskCreate(
        espnow_send_task, "tbft_send",
        4096, enow,
        SEND_TASK_PRIORITY,
        &enow->send_task_handle);
    if (ret != pdPASS) {
        enow->send_task_handle = NULL;
        ESP_LOGE(TAG, "failed to create send task");
        vQueueDelete(enow->send_queue);
        vQueueDelete(enow->msg_queue);
        vSemaphoreDelete(enow->lock);
        free(enow);
        return -1;
    }

    *out = (tbft_transport_t *)enow;
    ESP_LOGI(TAG, "ESP-NOW transport init: max_frag=%d, max_parts=%d, send_task=0x%p",
             FRAG_MAX_PAYLOAD, FRAG_MAX_PARTS, enow->send_task_handle);
    return 0;
}

void tbft_transport_free(tbft_transport_t *t)
{
    if (!t) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    if (enow->send_task_handle) {
        vTaskDelete(enow->send_task_handle);
    }
    if (enow->send_queue) {
        vQueueDelete(enow->send_queue);
    }
    if (enow->msg_queue) {
        vQueueDelete(enow->msg_queue);
    }
    if (enow->lock) {
        vSemaphoreDelete(enow->lock);
    }
    g_espnow_ctx = NULL;
    free(enow);
}

void tbft_transport_set_peer(tbft_transport_t *t,
                             tbft_node_id_t node_id,
                             const tbft_addr_t *addr)
{
    if (!t || node_id < 0
            || node_id >= TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS)
        return;

    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    enow->peers[node_id] = *addr;
    enow->peer_valid[node_id] = true;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, addr->u.mac.bytes, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_now_del_peer(addr->u.mac.bytes);
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer for node %d failed: %d", node_id, err);
    } else {
        ESP_LOGI(TAG, "ESP-NOW peer %d: " MACSTR, node_id, MAC2STR(addr->u.mac.bytes));
    }
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len,
                        tbft_node_id_t dest)
{
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || !enow->send_queue) return -1;
    if (len > TBFT_MAX_MESSAGE_SIZE) return -1;

    send_entry_t entry;
    memcpy(entry.buf, buf, len);
    entry.len  = (int)len;
    entry.dest = dest;

    /* Non-blocking post to send queue — returns immediately */
    if (xQueueSend(enow->send_queue, &entry, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "send_queue full");
        return -1;
    }
    return (int)len;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len,
                        tbft_node_id_t *src_id)
{
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow) return -1;

    uint8_t q_entry[6 + 4 + TBFT_MAX_MESSAGE_SIZE];

    /* Block until a message arrives or the WDT timeout expires */
    if (xQueueReceive(enow->msg_queue, q_entry, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;  /* timeout — nothing available */
    }

    const uint8_t *src_mac = q_entry;
    int msg_len = 0;
    memcpy(&msg_len, q_entry + 6, 4);

    if (msg_len < 0 || (size_t)msg_len > buf_len) {
        ESP_LOGE(TAG, "recv: message too large: %d", msg_len);
        return -1;
    }

    memcpy(buf, q_entry + 10, (size_t)msg_len);

    if (src_id) {
        *src_id = find_node_by_mac(enow, src_mac);
    }

    return msg_len;
}
