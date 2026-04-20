/**
 * @file tbft_transport_espnow.c
 * @brief แบ็กเอนด์การส่งข้อมูลผ่าน ESP-NOW พร้อมระบบแบ่งส่วน/ประกอบกลับข้อมูลอัตโนมัติ (fragmentation/reassembly)
 */

#include "tbft_transport.h"
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
#define FRAG_MAX_PARTS      ((TBFT_MAX_MESSAGE_SIZE + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD + 1)
#define REASM_MAX_SLOTS     4
#define REASM_TIMEOUT_MS    5000
#define FRAG_INTER_DELAY_MS 5

#define MSG_QUEUE_DEPTH     8
#define SEND_QUEUE_DEPTH    4
#define SEND_TASK_PRIORITY  3
#define SEND_TASK_STACK_SIZE 4096
#define SEND_TASK_RECV_TIMEOUT_MS  50
#define RECV_TASK_TIMEOUT_MS  100
#define SEND_QUEUE_TIMEOUT_MS 10
#define SHUTDOWN_DRAIN_MS     200

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

static void reasm_clear_stale(tbft_espnow_t *enow) {
    for (int i = 0; i < REASM_MAX_SLOTS; i++) {
        reasm_slot_t *s = &enow->reasm[i];
        if (s->stale) {
            memset(s, 0, sizeof(*s));
            s->stale = false;
        }
    }
}

static TickType_t now_ticks(void) {
    return xTaskGetTickCount();
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
            s->stale = true;
            enow->reasm_stale_flag = true;
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
    enow->reasm[oldest].stale = true;
    enow->reasm_stale_flag = true;
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

    if (msg_id != s->msg_id || frag_total != s->frag_total) return false;
    if (frag_idx >= frag_total || frag_idx >= 32) return false;
    if (s->frag_mask & (1U << frag_idx)) return false;

    int offset = (int)frag_idx * FRAG_MAX_PAYLOAD;
    if (offset + data_len > TBFT_MAX_MESSAGE_SIZE) return false;

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
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int data_len) {
    if (!g_espnow_ctx || !recv_info || !data) return;
    if (data_len < (int)sizeof(frag_hdr_t)) return;

    tbft_espnow_t *enow = g_espnow_ctx;
    const uint8_t *src_mac = recv_info->src_addr;
    const frag_hdr_t *fhdr = (const frag_hdr_t *)data;

    /* รอ 10ms ป้องกันการ Drop Packet หาก Lock ถูก Task อื่นใช้งานอยู่แวบเดียว */
    if (xSemaphoreTake(enow->lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "recv: Lock timeout, packet dropped");
        return;
    }

    reasm_slot_t *s = reasm_find_or_alloc(enow, fhdr->msg_id, src_mac);
    if (!s) {
        xSemaphoreGive(enow->lock);
        return;
    }

    const uint8_t *payload = data + sizeof(frag_hdr_t);
    int payload_len = data_len - (int)sizeof(frag_hdr_t);

    bool complete = reasm_feed(enow, s, payload, payload_len, fhdr->msg_id, fhdr->frag_idx, fhdr->frag_total, src_mac);

    if (complete) {
        recv_entry_t q_entry;
        memcpy(q_entry.src_mac, src_mac, ESPNOW_MAC_LEN);
        q_entry.buf_len = s->buf_len;
        memcpy(q_entry.payload, s->buf, (size_t)s->buf_len);

        if (xQueueSend(enow->msg_queue, &q_entry, 0) != pdTRUE) {
            ESP_LOGW(TAG, "msg_queue full, dropping reassembled msg");
        }

        s->stale = true;
        enow->reasm_stale_flag = true;
    }

    xSemaphoreGive(enow->lock);
}

/* --------------------------------------------------------------------------
 * ฟังก์ชันส่งข้อมูลและ Send Task
 * -------------------------------------------------------------------------- */

static int espnow_do_send(tbft_espnow_t *enow, const uint8_t *peer_mac, uint16_t msg_id, const uint8_t *buf, size_t len) {
    if (!peer_mac || !buf || len == 0) return -1;

    uint8_t ap_mac[6];
    get_ap_mac(peer_mac, ap_mac); /* คำนวณแบบป้องกัน Overflow */

    if (len + sizeof(frag_hdr_t) <= ESPNOW_MAX_DATA_LEN) {
        frag_hdr_t fhdr = { .msg_id = msg_id, .frag_total = 1, .frag_idx = 0 };
        uint8_t pkt[sizeof(frag_hdr_t) + ESPNOW_MAX_DATA_LEN];
        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), buf, len);

        if (esp_now_send(ap_mac, pkt, len + sizeof(fhdr)) != ESP_OK) return -1;
        vTaskDelay(pdMS_TO_TICKS(FRAG_INTER_DELAY_MS));
        return (int)len;
    }

    size_t remaining = len;
    const uint8_t *src = buf;
    uint8_t frag_total = (uint8_t)((remaining + FRAG_MAX_PAYLOAD - 1) / FRAG_MAX_PAYLOAD);
    uint8_t frag_idx = 0;

    frag_hdr_t fhdr = { .msg_id = msg_id, .frag_total = frag_total };
    uint8_t pkt[sizeof(frag_hdr_t) + FRAG_MAX_PAYLOAD];

    while (remaining > 0) {
        fhdr.frag_idx = frag_idx;
        size_t chunk = remaining > FRAG_MAX_PAYLOAD ? FRAG_MAX_PAYLOAD : remaining;

        memcpy(pkt, &fhdr, sizeof(fhdr));
        memcpy(pkt + sizeof(fhdr), src, chunk);

        if (esp_now_send(ap_mac, pkt, chunk + sizeof(fhdr)) != ESP_OK) return -1;
        vTaskDelay(pdMS_TO_TICKS(FRAG_INTER_DELAY_MS));

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
            if (enow->reasm_stale_flag) {
                enow->reasm_stale_flag = false;
                xSemaphoreTake(enow->lock, portMAX_DELAY);
                reasm_clear_stale(enow);
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
            for (int i = 0; i < limit; i++) {
                if (i == enow->local_id) continue;

                xSemaphoreTake(enow->lock, portMAX_DELAY);
                bool valid = enow->peer_valid[i];
                tbft_addr_t peer_addr = enow->peers[i];
                uint16_t this_msg_id = enow->next_msg_id++;
                xSemaphoreGive(enow->lock);

                if (valid) espnow_do_send(enow, peer_addr.u.mac.bytes, this_msg_id, entry.buf, (size_t)entry.len);
            }
        } else if (entry.dest >= 0 && entry.dest < enow->num_nodes) {
            xSemaphoreTake(enow->lock, portMAX_DELAY);
            bool valid = enow->peer_valid[entry.dest];
            tbft_addr_t peer_addr = enow->peers[entry.dest];
            uint16_t this_msg_id = enow->next_msg_id++;
            xSemaphoreGive(enow->lock);

            if (valid) espnow_do_send(enow, peer_addr.u.mac.bytes, this_msg_id, entry.buf, (size_t)entry.len);
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

    if (!enow->lock || !enow->task_exit_sem || !enow->msg_queue || !enow->send_queue) {
        ESP_LOGE(TAG, "queue/sem alloc failed: lock=%p sem=%p msg_q=%p send_q=%p free_heap=%u",
                 enow->lock, enow->task_exit_sem, enow->msg_queue, enow->send_queue,
                 (unsigned)esp_get_free_heap_size());
        if (enow->send_queue) vQueueDelete(enow->send_queue);
        if (enow->msg_queue) vQueueDelete(enow->msg_queue);
        if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
        if (enow->lock) vSemaphoreDelete(enow->lock);
        free(enow);
        return -1;
    }

    xSemaphoreGive(enow->lock);
    g_espnow_ctx = enow;

    esp_err_t rcb_err = esp_now_register_recv_cb(espnow_recv_cb);
    if (rcb_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_register_recv_cb failed: 0x%x (%s) — "
                 "ensure esp_now_init() was called before Byz_init_replica",
                 rcb_err, esp_err_to_name(rcb_err));
        goto init_error;
    }
    esp_now_register_send_cb(espnow_send_cb);

    if (xTaskCreate(espnow_send_task, "tbft_send", SEND_TASK_STACK_SIZE, enow, SEND_TASK_PRIORITY, &enow->send_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate for send_task failed");
        goto init_error;
    }

    *out = (tbft_transport_t *)enow;
    return 0;

init_error:
    g_espnow_ctx = NULL;
    vQueueDelete(enow->send_queue);
    vQueueDelete(enow->msg_queue);
    vSemaphoreDelete(enow->task_exit_sem);
    vSemaphoreDelete(enow->lock);
    free(enow);
    return -1;
}

void tbft_transport_free(tbft_transport_t *t) {
    if (!t) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    enow->shutting_down = true;
    esp_now_unregister_recv_cb();
    g_espnow_ctx = NULL;

    vTaskDelay(pdMS_TO_TICKS(50));

    if (enow->send_queue && enow->send_task_handle) {
        send_entry_t sentinel = { .dest = SEND_TASK_SHUTDOWN };
        if (xQueueSend(enow->send_queue, &sentinel, pdMS_TO_TICKS(SHUTDOWN_DRAIN_MS)) == pdTRUE) {
            /* รอจนกว่า send_task จะสั่งปิดตัวเองเสร็จสิ้นอย่างปลอดภัย */
            xSemaphoreTake(enow->task_exit_sem, pdMS_TO_TICKS(1000));
        }
    }

    if (enow->send_queue) vQueueDelete(enow->send_queue);
    if (enow->msg_queue) vQueueDelete(enow->msg_queue);
    if (enow->task_exit_sem) vSemaphoreDelete(enow->task_exit_sem);
    if (enow->lock) vSemaphoreDelete(enow->lock);
    free(enow);
}

void tbft_transport_set_peer(tbft_transport_t *t, tbft_node_id_t node_id, const tbft_addr_t *addr) {
    if (!t || node_id < 0 || node_id >= TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) return;
    tbft_espnow_t *enow = (tbft_espnow_t *)t;

    xSemaphoreTake(enow->lock, portMAX_DELAY);
    enow->peers[node_id] = *addr;
    enow->peer_valid[node_id] = true;

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

    esp_now_peer_info_t peer = { .channel = 1, .ifidx = WIFI_IF_AP, .encrypt = false };
    memcpy(peer.peer_addr, ap_mac, 6);

    esp_now_del_peer(ap_mac);
    esp_err_t add_ret = esp_now_add_peer(&peer);
    if (add_ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_add_peer for node %d failed: 0x%x", node_id, add_ret);
    }
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len, tbft_node_id_t dest) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || !enow->send_queue || enow->shutting_down) return -1;
    if (len > (size_t)INT_MAX || len > TBFT_MAX_MESSAGE_SIZE) return -1;

    send_entry_t entry;
    memcpy(entry.buf, buf, len);
    entry.len  = (int)len;
    entry.dest = dest;

    if (xQueueSend(enow->send_queue, &entry, pdMS_TO_TICKS(SEND_QUEUE_TIMEOUT_MS)) != pdTRUE) return -1;
    return (int)len;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len, tbft_node_id_t *src_id) {
    tbft_espnow_t *enow = (tbft_espnow_t *)t;
    if (!enow || enow->shutting_down) return -1;

    if (enow->reasm_stale_flag) {
        enow->reasm_stale_flag = false;
        xSemaphoreTake(enow->lock, portMAX_DELAY);
        reasm_clear_stale(enow);
        xSemaphoreGive(enow->lock);
    }

    recv_entry_t entry;
    if (xQueueReceive(enow->msg_queue, &entry, pdMS_TO_TICKS(SEND_TASK_RECV_TIMEOUT_MS)) != pdTRUE) return 0;

    if (entry.buf_len < 0 || (size_t)entry.buf_len > buf_len || (size_t)entry.buf_len > TBFT_MAX_MESSAGE_SIZE) return -1;

    memcpy(buf, entry.payload, (size_t)entry.buf_len);
    if (src_id) *src_id = find_node_by_mac(enow, entry.src_mac);

    return entry.buf_len;
}
