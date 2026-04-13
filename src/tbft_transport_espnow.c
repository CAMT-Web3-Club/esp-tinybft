/**
 * @file tbft_transport_espnow.c
 * @brief ESP-NOW transport backend with automatic fragmentation/reassembly.
 *
 * ESP-NOW v2.0 supports up to 1470 bytes per packet.  Messages larger than
 * this are automatically fragmented on send and reassembled on receive.
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

/* ESP-NOW v2.0 max data length (ESP-IDF v5+) */
#ifndef ESPNOW_MAX_DATA_LEN
#define ESPNOW_MAX_DATA_LEN  ESP_NOW_MAX_DATA_LEN_V2  /* 1470 */
#endif

/** Fragment header size */
#define FRAG_HDR_SIZE  4

/** Max useful payload per fragment */
#define FRAG_MAX_PAYLOAD  (ESPNOW_MAX_DATA_LEN - FRAG_HDR_SIZE)

/** Maximum fragments for a TBFT_MAX_MESSAGE_SIZE message */
#define FRAG_MAX_PARTS  ((TBFT_MAX_MESSAGE_SIZE + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD + 1)

/** Max concurrent reassembly slots */
#define REASM_MAX_SLOTS  4

/** Reassembly timeout in ticks */
#define REASM_TIMEOUT_MS  5000

/* --------------------------------------------------------------------------
 * Fragment header layout
 * -------------------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct {
    uint16_t msg_id;
    uint8_t  frag_idx;
    uint8_t  frag_total;
} frag_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(frag_hdr_t) == FRAG_HDR_SIZE, "frag_hdr_t size mismatch");

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

typedef struct {
    /* Reassembly slot: holds fragments for one in-progress message */
    uint8_t   buf[TBFT_MAX_MESSAGE_SIZE];
    int       buf_len;
    uint16_t  msg_id;
    uint8_t   frag_total;
    uint8_t   frag_received;  /* bitmap — up to 32 fragments via uint32_t */
    uint32_t  frag_mask;      /* bitmask of received fragment indices */
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

    /* Reassembly slots */
    reasm_slot_t  reasm[REASM_MAX_SLOTS];

    /* Monotonic message ID counter */
    uint16_t next_msg_id;

    /* Mutex for concurrent access */
    SemaphoreHandle_t lock;

    /* Send status — updated by ESP-NOW send callback */
    volatile bool send_done;
    volatile bool send_ok;
} tbft_espnow_t;

/* Global pointer for ESP-NOW callbacks (single transport instance) */
static tbft_espnow_t *g_espnow_ctx = NULL;

/* --------------------------------------------------------------------------
 * Reassembly helpers
 * -------------------------------------------------------------------------- */

static TickType_t now_ticks(void)
{
    return xTaskGetTickCount();
}

/** Find an existing slot for this (msg_id, src_mac), or a free one. */
static reasm_slot_t *reasm_find_or_alloc(tbft_espnow_t *enow,
                                          uint16_t msg_id,
                                          const uint8_t *src_mac)
{
    /* Check existing slots for same message */
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (s->valid && s->msg_id == msg_id
                && memcmp(s->src_mac, src_mac, 6) == 0) {
            return s;
        }
    }

    /* Find a free slot, evicting timed-out ones */
    TickType_t now = now_ticks();
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (!s->valid) return s;  /* free slot */
        TickType_t elapsed = now - s->last_tick;
        if (elapsed > pdMS_TO_TICKS(REASM_TIMEOUT_MS)) {
            /* Timed out — evict */
            memset(s, 0, sizeof(*s));
            return s;
        }
    }

    /* All slots busy and not timed out — evict oldest (smallest last_tick) */
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

/** Feed a fragment into a reassembly slot.  Returns true when fully reassembled. */
static bool reasm_feed(reasm_slot_t *s, const uint8_t *data, int data_len,
                       uint16_t msg_id, uint8_t frag_idx, uint8_t frag_total,
                       const uint8_t *src_mac)
{
    if (!s->valid) {
        /* New message */
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
    if (s->frag_mask & (1U << frag_idx)) return false;  /* duplicate */

    int offset = (int)frag_idx * FRAG_MAX_PAYLOAD;
    if (offset + data_len > TBFT_MAX_MESSAGE_SIZE) return false;

    memcpy(s->buf + offset, data, (size_t)data_len);
    s->frag_mask |= (1U << frag_idx);
    s->frag_received++;
    s->last_tick = now_ticks();

    if (s->frag_received == frag_total) {
        /* All fragments received */
        s->buf_len = offset + data_len;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * ESP-NOW callbacks (called from Wi-Fi task context)
 * -------------------------------------------------------------------------- */

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                            const uint8_t *data, int data_len)
{
    if (!g_espnow_ctx || !data || data_len < (int)sizeof(frag_hdr_t)) return;

    const uint8_t *src_mac = recv_info->src_addr;
    const frag_hdr_t *fhdr = (const frag_hdr_t *)data;

    tbft_espnow_t *enow = g_espnow_ctx;
    if (xSemaphoreTake(enow->lock, pdMS_TO_TICKS(100)) != pdTRUE) return;

    reasm_slot_t *s = reasm_find_or_alloc(enow, fhdr->msg_id, src_mac);
    if (!s) { xSemaphoreGive(enow->lock); return; }

    const uint8_t *payload = data + sizeof(frag_hdr_t);
    int payload_len = data_len - (int)sizeof(frag_hdr_t);

    bool complete = reasm_feed(s, payload, payload_len,
                               fhdr->msg_id, fhdr->frag_idx,
                               fhdr->frag_total, src_mac);

    if (complete) {
        /* Push to message queue (copy src_mac as prefix for identification) */
        /* Queue stores: {src_mac[6], buf_len (int), buf[]} */
        uint8_t q_entry[6 + 4 + TBFT_MAX_MESSAGE_SIZE];
        memcpy(q_entry, src_mac, 6);
        memcpy(q_entry + 6, &s->buf_len, 4);
        memcpy(q_entry + 10, s->buf, (size_t)s->buf_len);

        if (xQueueSend(enow->msg_queue, q_entry, 0) != pdTRUE) {
            ESP_LOGW(TAG, "msg_queue full, dropping reassembled msg");
        }
        memset(s, 0, sizeof(*s));  /* free slot */
    }

    xSemaphoreGive(enow->lock);
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info,
                            esp_now_send_status_t status)
{
    (void)tx_info;
    if (g_espnow_ctx) {
        g_espnow_ctx->send_done = true;
        g_espnow_ctx->send_ok = (status == ESP_NOW_SEND_SUCCESS);
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
    (void)type;     /* only called with TBFT_TRANSPORT_ESPNOW */
    (void)mcast_ip; /* not used for ESP-NOW */
    (void)port;     /* not used for ESP-NOW */

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

    /* Register global context (only one ESP-NOW transport at a time) */
    g_espnow_ctx = enow;

    /* Register ESP-NOW callbacks */
    esp_now_register_recv_cb(espnow_recv_cb);
    esp_now_register_send_cb(espnow_send_cb);

    *out = (tbft_transport_t *)enow;
    ESP_LOGI(TAG, "ESP-NOW transport init: max_frag=%d, max_parts=%d",
             FRAG_MAX_PAYLOAD, FRAG_MAX_PARTS);
    return 0;
}

void tbft_transport_free(tbft_transport_t *t)
{
    if (!t) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    if (enow->msg_queue) {
        vQueueDelete(enow->msg_queue);
    }
    if (enow->lock) {
        vSemaphoreDelete(enow->lock);
    }
    if (g_espnow_ctx == enow) {
        g_espnow_ctx = NULL;
    }
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

    /* Register peer with ESP-NOW */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, addr->u.mac.bytes, 6);
    peer.channel = 0;  /* use current WiFi channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_now_del_peer(addr->u.mac.bytes);  /* ignore error if not registered */
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_add_peer for node %d failed: %d",
                 node_id, err);
    } else {
        ESP_LOGI(TAG, "ESP-NOW peer %d: " MACSTR, node_id,
                 MAC2STR(addr->u.mac.bytes));
    }
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len,
                        tbft_node_id_t dest)
{
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow) return -1;

    /* Broadcast to all replicas */
    if (dest == TBFT_ALL_REPLICAS) {
        int sent = 0;
        for (int i = 0; i < enow->num_nodes; i++) {
            if (!enow->peer_valid[i]) continue;
            const uint8_t *peer_mac = enow->peers[i].u.mac.bytes;

            uint16_t this_msg_id = enow->next_msg_id;
            enow->next_msg_id++;

            /* Check if fragmentation is needed */
            if (len + sizeof(frag_hdr_t) <= ESPNOW_MAX_DATA_LEN) {
                /* Single fragment */
                frag_hdr_t fhdr;
                fhdr.msg_id     = this_msg_id;
                fhdr.frag_total = 1;
                fhdr.frag_idx   = 0;

                uint8_t pkt[sizeof(frag_hdr_t) + ESPNOW_MAX_DATA_LEN];
                memcpy(pkt, &fhdr, sizeof(fhdr));
                memcpy(pkt + sizeof(fhdr), buf, len);

                enow->send_done = false;
                enow->send_ok = false;

                esp_err_t err = esp_now_send(peer_mac, pkt,
                                             (size_t)len + sizeof(fhdr));
                if (err == ESP_OK) {
                    int retries = 0;
                    while (!enow->send_done && retries < 100) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        retries++;
                    }
                    if (enow->send_ok) sent = (int)len;
                } else {
                    ESP_LOGE(TAG, "esp_now_send to peer %d failed: %d", i, err);
                }
            } else {
                /* Multi-fragment send to this peer */
                size_t remaining = len;
                const uint8_t *src = (const uint8_t *)buf;
                uint8_t frag_total = (uint8_t)((remaining + FRAG_MAX_PAYLOAD - 1)
                                               / FRAG_MAX_PAYLOAD);
                uint8_t frag_idx = 0;
                bool peer_ok = true;

                frag_hdr_t fhdr;
                fhdr.msg_id = this_msg_id;
                fhdr.frag_total = frag_total;

                uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD];

                while (remaining > 0 && peer_ok) {
                    fhdr.frag_idx = frag_idx;
                    size_t chunk = remaining > FRAG_MAX_PAYLOAD
                                 ? FRAG_MAX_PAYLOAD : remaining;

                    memcpy(pkt, &fhdr, sizeof(fhdr));
                    memcpy(pkt + sizeof(fhdr), src, chunk);

                    enow->send_done = false;
                    enow->send_ok = false;

                    esp_err_t err = esp_now_send(peer_mac, pkt,
                                                 chunk + sizeof(fhdr));
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_now_send bcast frag %d/%d to %d failed: %d",
                                 frag_idx, frag_total, i, err);
                        peer_ok = false;
                        break;
                    }

                    int retries = 0;
                    while (!enow->send_done && retries < 100) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                        retries++;
                    }
                    if (!enow->send_ok) {
                        ESP_LOGE(TAG, "esp_now_send bcast frag %d/%d to %d cb fail",
                                 frag_idx, frag_total, i);
                        peer_ok = false;
                        break;
                    }

                    src += chunk;
                    remaining -= chunk;
                    frag_idx++;
                }
                if (peer_ok) sent = (int)len;
            }
        }
        return sent > 0 ? sent : -1;
    }

    /* Unicast */
    if (dest < 0 || dest >= enow->num_nodes || !enow->peer_valid[dest]) {
        ESP_LOGE(TAG, "send: peer %d not registered", dest);
        return -1;
    }

    const uint8_t *peer_mac = enow->peers[dest].u.mac.bytes;

    /* Check if fragmentation is needed */
    if (len + sizeof(frag_hdr_t) <= ESPNOW_MAX_DATA_LEN) {
        /* Single fragment — pkt only needs to hold one fragment */
        uint8_t pkt[sizeof(frag_hdr_t) + ESPNOW_MAX_DATA_LEN];
        frag_hdr_t fhdr;
        fhdr.msg_id    = enow->next_msg_id;
        fhdr.frag_total = 1;
        fhdr.frag_idx  = 0;

        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), buf, len);

        enow->send_done = false;
        enow->send_ok = false;

        esp_err_t err = esp_now_send(peer_mac, pkt, len + sizeof(fhdr));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send failed: %d", err);
            return -1;
        }

        int retries = 0;
        while (!enow->send_done && retries < 100) {
            vTaskDelay(pdMS_TO_TICKS(1));
            retries++;
        }

        enow->next_msg_id++; /* must increment even for single-fragment messages */
        return enow->send_ok ? (int)len : -1;
    }

    /* Multi-fragment send */
    size_t remaining = len;
    const uint8_t *src = (const uint8_t *)buf;
    uint8_t frag_total = (uint8_t)((remaining + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD);
    uint8_t frag_idx = 0;

    frag_hdr_t fhdr;
    fhdr.msg_id = enow->next_msg_id;
    fhdr.frag_total = frag_total;

    uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD];

    while (remaining > 0) {
        fhdr.frag_idx = frag_idx;
        size_t chunk = remaining > FRAG_MAX_PAYLOAD ? FRAG_MAX_PAYLOAD : remaining;

        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), src, chunk);

        enow->send_done = false;
        enow->send_ok = false;

        esp_err_t err = esp_now_send(peer_mac, pkt, chunk + sizeof(fhdr));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send frag %d/%d failed: %d",
                     frag_idx, frag_total, err);
            return -1;
        }

        int retries = 0;
        while (!enow->send_done && retries < 100) {
            vTaskDelay(pdMS_TO_TICKS(1));
            retries++;
        }
        if (!enow->send_ok) {
            ESP_LOGE(TAG, "esp_now_send frag %d/%d failed (callback)",
                     frag_idx, frag_total);
            return -1;
        }

        src += chunk;
        remaining -= chunk;
        frag_idx++;
    }

    enow->next_msg_id++;
    return (int)len;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len,
                        tbft_node_id_t *src_id)
{
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow) return -1;

    /* Check queue for a fully reassembled message */
    uint8_t q_entry[6 + 4 + TBFT_MAX_MESSAGE_SIZE];
    if (xQueueReceive(enow->msg_queue, q_entry, 0) != pdTRUE) {
        return 0;  /* nothing available */
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
