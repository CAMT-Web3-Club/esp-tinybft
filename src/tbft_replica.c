/**
 * @file tbft_replica.c
 * @brief PBFT state machine — message dispatch, handlers, and replica lifecycle.
 *
 * Implements the core TinyBFT replica logic:
 *   - Request handling (deduplication, queueing, forwarding)
 *   - Pre-prepare / Prepare / Commit message processing
 *   - Checkpoint management and state transfer triggers
 *   - View-change protocol (collect, verify, new-view)
 *   - HMAC session key exchange (New_key)
 *
 * The replica runs as a single event loop (tbft_replica_run) that drains
 * messages from the transport layer and dispatches them by tag. All heavy
 * cryptographic operations (RSA) run in the loop context; timer callbacks
 * only set event bits to avoid blocking ISR/task contexts.
 *
 * Memory model: Three statically-allocated regions (agreement, checkpoint,
 * special) are embedded directly in the replica struct — no heap allocation
 * in the steady-state protocol path.
 */

#include "tbft_replica.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "tbft_replica";

/* Low #8 FIX: Named constants for magic numbers */
#define TBFT_INIT_DRAIN_LIMIT     64   /* max messages to drain after New_key broadcast */
#define TBFT_KEY_DRAIN_LIMIT      32   /* max messages to drain during key confirmation */
#define TBFT_YIELD_THRESHOLD      16   /* message loop iterations before cooperative yield */

/* M2 FIX: Read-only request flag in hdr.extra field */
#define TBFT_REQUEST_RO_FLAG      0x1

/* --------------------------------------------------------------------------
 * Request queue helpers
 * -------------------------------------------------------------------------- */

static void rqueue_init(tbft_rqueue_t *q) {
    memset(q, 0, sizeof(*q));
}

static bool rqueue_push(tbft_rqueue_t *q, const void *buf, int len, bool ro) {
    if (q->count >= TBFT_RQUEUE_MAX) return false;
    if (len < 0 || len > TBFT_MAX_MESSAGE_SIZE) return false;
    tbft_rqueue_entry_t *e = &q->entries[q->tail];
    memcpy(e->buf, buf, (size_t)len);
    e->len  = len;
    e->ro   = ro;
    e->used = true;
    q->tail = (q->tail + 1) % TBFT_RQUEUE_MAX;
    q->count++;
    return true;
}

static tbft_rqueue_entry_t *rqueue_front(tbft_rqueue_t *q) {
    if (q->count == 0) return NULL;
    return &q->entries[q->head];
}

static void rqueue_pop(tbft_rqueue_t *q) {
    if (q->count == 0) return;
    q->entries[q->head].used = false;
    q->head = (q->head + 1) % TBFT_RQUEUE_MAX;
    q->count--;
}

/* --------------------------------------------------------------------------
 * Timer callbacks
 * -------------------------------------------------------------------------- */

static void vtimer_cb(void *arg)
{
    tbft_replica_t *r = (tbft_replica_t *)arg;
    if (r->evt_group) {
        xEventGroupSetBits(r->evt_group, TBFT_EVT_VTIMER);
    }
}

static void stimer_cb(void *arg)
{
    tbft_replica_t *r = (tbft_replica_t *)arg;
    if (r->evt_group) {
        xEventGroupSetBits(r->evt_group, TBFT_EVT_STIMER);
    }
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

int tbft_replica_init(tbft_replica_t *r,
                      tbft_node_id_t node_id, int f, int num_nodes,
                      const char *mcast_ip, int64_t auth_timeout_us,
                      uint16_t port,
                      void *state_mem, size_t state_size,
                      tbft_exec_cb_t exec_cb,
                      tbft_comp_ndet_cb_t comp_ndet_cb,
                      int ndet_max_len,
                      tbft_recv_reply_cb_t recv_reply_cb)
{
    memset(r, 0, sizeof(*r));

    /* Init base node */
    if (tbft_node_init(&r->node, node_id, f, num_nodes,
                       mcast_ip, auth_timeout_us, port) != 0) {
        return -1;
    }

    int n = 3 * f + 1;
    int prepare_threshold = 2 * f;   /* 2f prepares (primary's PP counts) */
    int commit_threshold  = 2 * f + 1;

    /* Init static memory regions */
    tbft_ar_init(&r->ar, prepare_threshold, commit_threshold);
    tbft_cr_init(&r->cr, n, commit_threshold);
    int num_clients = num_nodes - n;
    if (num_clients < 1) num_clients = 1;
    tbft_sr_init(&r->sr, n, num_clients);

    /* Init state */
    if (tbft_state_init(&r->state, state_mem, state_size) != 0) {
        tbft_node_free(&r->node);
        return -1;
    }

    /* Init view-change info */
    tbft_vi_init(&r->vi, n, commit_threshold, &r->sr);

    /* Init request queues */
    rqueue_init(&r->rqueue);
    rqueue_init(&r->ro_rqueue);

    /* Set callbacks */
    r->exec_cb        = exec_cb;
    r->comp_ndet_cb   = comp_ndet_cb;
    r->ndet_max_len   = (ndet_max_len > TBFT_NDET_BUF_SIZE)
                      ? TBFT_NDET_BUF_SIZE : ndet_max_len;
    r->recv_reply_cb  = recv_reply_cb;

    /* Sequence number initialisation */
    r->seqno                    = 1;
    r->last_stable              = 0;
    r->last_prepared            = 0;
    r->last_executed            = 0;
    r->last_tentative_execute   = 0;

    /* Timer periods (defaults — configurable via menuconfig, overridden by
     * Byz_init_replica from config file). */
    r->vtimer_period_us = TBFT_VIEW_CHANGE_TIMEOUT_US;
    r->stimer_period_us = TBFT_STATUS_TIMEOUT_US;

    /* Init timers (not started — caller must set periods and start).
     * NOTE: rtimer (recovery) and ntimer (keep-alive) are reserved for
     * future use. They are not initialised or freed to avoid allocating
     * unused esp_timer handles. */
    tbft_itimer_init(&r->vtimer, vtimer_cb, r, "tbft_vtimer");
    tbft_itimer_init(&r->stimer, stimer_cb, r, "tbft_stimer");

    r->evt_group = xEventGroupCreate();
    if (!r->evt_group) {
        ESP_LOGE(TAG, "replica: failed to create event group");
        tbft_itimer_free(&r->vtimer);
        tbft_itimer_free(&r->stimer);
        /* rtimer/ntimer not initialised — nothing to free */
        tbft_state_free(&r->state);
        tbft_node_free(&r->node);
        return -1;
    }

    ESP_LOGI(TAG, "replica %d init: f=%d n=%d state=%zu bytes",
             node_id, f, n, state_size);
    return 0;
}

void tbft_replica_free(tbft_replica_t *r)
{
    r->running = false;
    tbft_itimer_free(&r->vtimer);
    tbft_itimer_free(&r->stimer);
    /* rtimer/ntimer are not initialised — nothing to free */
    if (r->evt_group) {
        vEventGroupDelete(r->evt_group);
        r->evt_group = NULL;
    }
    tbft_state_free(&r->state);
    tbft_node_free(&r->node);
}

/* --------------------------------------------------------------------------
 * Message dispatch
 * -------------------------------------------------------------------------- */

void tbft_replica_run(tbft_replica_t *r)
{
    r->running = true;
    ESP_LOGI(TAG, "replica %d starting event loop", r->node.node_id);

    /* Broadcast fresh HMAC session keys to all peers.  Peers will also
     * send their own New_key messages which we handle in the main loop.
     * We do NOT block waiting for keys — boards may start at different
     * times (e.g. sequential flashing), so we enter the main loop
     * immediately and handle New_key asynchronously. */
    tbft_replica_send_new_key(r);

    /* Drain ALL messages that arrived during init.  This is critical —
     * we cannot silently discard View_change, Prepare, Commit, Checkpoint
     * or other messages.  Non-NEW_KEY messages are acknowledged with a log
     * to catch unexpected early arrivals. */
    int drained = 0;
    while (drained < TBFT_INIT_DRAIN_LIMIT) {
        tbft_node_id_t src_id = -1;
        int n = tbft_node_recv(&r->node, r->node.recv_buf, sizeof(r->node.recv_buf), &src_id);
        if (n < 0) break; /* transport error during drain */
        if (n < (int)sizeof(tbft_msg_hdr_t)) break;
        const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)r->node.recv_buf;
        if (n >= hdr->size && hdr->tag == TBFT_MSG_NEW_KEY) {
            tbft_replica_handle_new_key(r, r->node.recv_buf, n);
        }
        drained++;
    }

    /* Key confirmation barrier: ensure we have in-keys from threshold-1
     * peers before entering view 0.  Without this, a dropped New_key
     * causes asymmetric HMAC state → one-way communication failure →
     * protocol stall and infinite view-changes. */
    {
        int keys_ok = 0;
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            if (r->node.principals[i] && r->node.principals[i]->keys_fresh) {
                keys_ok++;
            }
        }
        if (keys_ok < r->node.threshold - 1) {
            ESP_LOGW(TAG, "only %d/%d HMAC keys ready after drain — retrying",
                     keys_ok, r->node.threshold - 1);
            tbft_replica_send_new_key(r);
            vTaskDelay(pdMS_TO_TICKS(200));
            drained = 0;
            while (drained < TBFT_KEY_DRAIN_LIMIT) {
                tbft_node_id_t src_id = -1;
                int n = tbft_node_recv(&r->node, r->node.recv_buf, sizeof(r->node.recv_buf), &src_id);
                if (n < 0) break; /* transport error during key drain */
                if (n < (int)sizeof(tbft_msg_hdr_t)) break;
                const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)r->node.recv_buf;
                if (n >= hdr->size && hdr->tag == TBFT_MSG_NEW_KEY) {
                    tbft_replica_handle_new_key(r, r->node.recv_buf, n);
                }
                drained++;
            }
        }
    }
    /* Subscribe this task to the task watchdog so we can periodically feed it.
     * The default timeout is CONFIG_ESP_TASK_WDT_TIMEOUT_S (typically 5s). */
#if CONFIG_ESP_TASK_WDT_EN
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to subscribe to task WDT: %d", err);
    }
#endif

    TickType_t last_nk_send = xTaskGetTickCount();

    while (r->running) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif

        /* Periodic yield counter: after processing a burst of messages,
         * yield to let lower-priority tasks (send_task at priority 3)
         * get CPU time.  Without this, the replica (priority 5) can
         * starve the send_task indefinitely under continuous message load.
         *
         * NOTE: This counter lives in the replica struct (not static) so
         * that restarting the replica correctly resets it. */
        if (++r->yield_counter >= TBFT_YIELD_THRESHOLD) {
            r->yield_counter = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        /* Periodically re-broadcast New_key to help late-joining nodes.
         * Send every 2s if we're missing keys, every 10s once ready. */
        {
            int keys_ok = 0;
            for (int i = 0; i < r->node.num_replicas; i++) {
                if (i == r->node.node_id) continue;
                if (r->node.principals[i] && r->node.principals[i]->keys_fresh) {
                    keys_ok++;
                }
            }
            TickType_t interval = (keys_ok >= r->node.threshold - 1)
                ? pdMS_TO_TICKS(10000) : pdMS_TO_TICKS(2000);
            TickType_t now = xTaskGetTickCount();
            if (now - last_nk_send >= interval) {
                tbft_replica_send_new_key(r);
                last_nk_send = now;
            }
        }

        if (r->evt_group) {
            EventBits_t bits = xEventGroupClearBits(r->evt_group, TBFT_EVT_VTIMER | TBFT_EVT_STIMER);
            /* Suppress view-change timeouts until we have enough HMAC keys.
             * Without keys, Prepare/Commit messages fail verification, so
             * view-changes would be triggered spuriously. */
            {
                int keys_ok = 0;
                for (int i = 0; i < r->node.num_replicas; i++) {
                    if (i == r->node.node_id) continue;
                    if (r->node.principals[i] && r->node.principals[i]->keys_fresh) {
                        keys_ok++;
                    }
                }
                if (keys_ok >= r->node.threshold - 1 && (bits & TBFT_EVT_VTIMER)) {
                    ESP_LOGW(TAG, "view-change timeout in view %lld (keys=%d/%d)",
                             (long long)r->node.view, keys_ok, r->node.threshold - 1);
                    tbft_replica_send_view_change(r);
                } else if (bits & TBFT_EVT_VTIMER) {
                    ESP_LOGD(TAG, "view-change suppressed: keys=%d/%d", keys_ok, r->node.threshold - 1);
                }
            }
            if (bits & TBFT_EVT_STIMER) {
                /* Broadcast a Status message */
                tbft_status_rep_t *st = (tbft_status_rep_t *)r->out_buf;
                st->hdr.tag           = TBFT_MSG_STATUS;
                st->hdr.extra         = 0;
                st->hdr.size          = tbft_msg_align((int32_t)sizeof(*st));
                st->view              = r->node.view;
                st->last_stable       = r->last_stable;
                st->last_prepared     = r->last_prepared;
                st->last_executed     = r->last_executed;
                st->id                = r->node.node_id;
                st->_pad              = 0;
                st->hdr.timestamp_us  = esp_timer_get_time();
                tbft_node_send(&r->node, r->out_buf, (size_t)st->hdr.size,
                               TBFT_ALL_REPLICAS);

                /* Restart status timer */
                tbft_itimer_start(&r->stimer, r->stimer_period_us);
            }
        }

        /* Check for fetch timeout — retry with different replier */
        if (tbft_state_in_fetch(&r->state) && tbft_state_fetch_timedout(&r->state)) {
            ESP_LOGW(TAG, "fetch timeout at seqno=%lld — restarting",
                     (long long)r->state.fetch_seqno);
            /* Pick a different replier */
            int new_replier = (r->state.fetch_replier + 1) % r->node.num_replicas;
            if (new_replier == r->node.node_id) {
                new_replier = (new_replier + 1) % r->node.num_replicas;
            }
            tbft_state_start_fetch(&r->state, r->state.fetch_seqno, new_replier);
        }

        tbft_node_id_t src_id = -1;
        int n = tbft_node_recv(&r->node, r->node.recv_buf, sizeof(r->node.recv_buf), &src_id);
        if (n < 0) {
            /* Transport error — log and yield before retrying */
            ESP_LOGW(TAG, "recv: transport error (id=%d)", r->node.node_id);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (n < (int)sizeof(tbft_msg_hdr_t)) {
            /* No message available — yield to let the WiFi task, lwIP,
             * and the IDLE task get CPU time on single-core MCUs. */
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)r->node.recv_buf;
        if (n < hdr->size) {
            ESP_LOGW(TAG, "truncated message: got %d, expected %d", n, hdr->size);
            continue;
        }
        if (hdr->size > (int32_t)TBFT_MAX_MESSAGE_SIZE || hdr->size <= 0) {
            ESP_LOGW(TAG, "invalid hdr->size=%d (n=%d)", (int)hdr->size, n);
            continue;
        }

        /* CRITICAL FIX: Until we have enough HMAC in-keys, only process
         * New_key messages.  All other protocol messages (View_change,
         * New_view, Prepare, Commit, etc.) require HMAC verification and
         * would either fail silently or corrupt state.  This prevents the
         * view-change cascade that occurs when nodes without keys exchange
         * view-change messages (which use RSA, not HMAC). */
        if (hdr->tag != TBFT_MSG_NEW_KEY) {
            int keys_ok = 0;
            for (int i = 0; i < r->node.num_replicas; i++) {
                if (i == r->node.node_id) continue;
                if (r->node.principals[i] && r->node.principals[i]->keys_fresh) {
                    keys_ok++;
                }
            }
            if (keys_ok < r->node.threshold - 1) {
                ESP_LOGD(TAG, "dropping msg tag=%d: keys=%d/%d",
                         hdr->tag, keys_ok, r->node.threshold - 1);
                continue; /* drop until keys ready */
            }
        }

        switch ((tbft_msg_tag_t)hdr->tag) {
        case TBFT_MSG_REQUEST:
            tbft_replica_handle_request(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_PRE_PREPARE:
            tbft_replica_handle_pre_prepare(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_PREPARE:
            tbft_replica_handle_prepare(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_COMMIT:
            tbft_replica_handle_commit(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_CHECKPOINT:
            tbft_replica_handle_checkpoint(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_VIEW_CHANGE:
            tbft_replica_handle_view_change(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_NEW_VIEW:
            tbft_replica_handle_new_view(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_FETCH:
            tbft_replica_handle_fetch(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_META_DATA:
            tbft_replica_handle_meta_data(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_DATA:
            tbft_replica_handle_data(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_NEW_KEY:
            tbft_replica_handle_new_key(r, r->node.recv_buf, n);
            break;
        default:
            ESP_LOGD(TAG, "unknown message tag %d", hdr->tag);
            break;
        }
    }

    /* HIGH FIX H3: Unsubscribe from task WDT when replica exits.
     * Without this, the WDT subsystem still expects feeds from a dead task,
     * eventually triggering a spurious system reset. */
#if CONFIG_ESP_TASK_WDT_EN
    esp_task_wdt_delete(NULL);
#endif
}

/* --------------------------------------------------------------------------
 * Request handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_request(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_request_rep_t)) return;
    const tbft_request_rep_t *req = (const tbft_request_rep_t *)msg;

    ESP_LOGI(TAG, "recv request: cid=%d rid=%llu cmd_size=%d (primary=%d)",
             req->cid, (unsigned long long)req->rid, req->command_size,
             tbft_replica_is_primary(r) ? 1 : 0);

    if (req->command_size < 0 ||
        (size_t)req->command_size > TBFT_MAX_MESSAGE_SIZE - sizeof(*req) - sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "request: invalid command_size %d", req->command_size);
        return;
    }

    if (req->cid < 0 || req->cid >= r->node.num_principals) {
        ESP_LOGW(TAG, "request: invalid client id %d (num_principals=%d)",
                 req->cid, r->node.num_principals);
        return;
    }

    /* Verify request signature */
    int sig_offset = (int)(sizeof(*req) + req->command_size);
    if (len < sig_offset + (int)sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "request: missing signature");
        return;
    }
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + sig_offset);
    if (!tbft_node_verify_sig(&r->node, (tbft_node_id_t)req->cid,
                              req, sig_offset, sig)) {
        ESP_LOGW(TAG, "request: signature verification failed from client %d",
                 req->cid);
        return;
    }

    /* If we are not the primary, forward to the primary only (unicast).
     * The client already broadcasts to all replicas, so the primary likely
     * received the request directly — this forward is a fallback in case
     * the client's direct send to the primary failed.  Unicasting (not
     * broadcasting) prevents the exponential forwarding storm that occurs
     * when every replica re-broadcasts every forwarded request. */
    if (!tbft_replica_is_primary(r)) {
        int primary = tbft_node_primary(&r->node, r->node.view);
        ESP_LOGD(TAG, "forwarding request to primary %d (view=%lld)",
                 primary, (long long)r->node.view);
        tbft_node_send(&r->node, msg, (size_t)len, primary);
        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
        return;
    }

    /* Primary: deduplicate by (cid, rid) before enqueuing.
     * Multiple copies of the same request can arrive — once from the
     * client's broadcast and again from each replica's forward. */
    bool ro = (req->hdr.extra & TBFT_REQUEST_RO_FLAG) != 0;
    tbft_rqueue_t *q = ro ? &r->ro_rqueue : &r->rqueue;
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % TBFT_RQUEUE_MAX;
        const tbft_request_rep_t *existing =
            (const tbft_request_rep_t *)q->entries[idx].buf;
        if (existing->cid == req->cid && existing->rid == req->rid) {
            ESP_LOGD(TAG, "request dedup: cid=%d rid=%llu", req->cid, (unsigned long long)req->rid);
            return;
        }
    }

    if (!rqueue_push(q, msg, len, ro)) {
        ESP_LOGW(TAG, "request queue full (count=%d), dropping request from client %d",
                 q->count, req->cid);
        return;
    }

    ESP_LOGI(TAG, "enqueued request: cid=%d rid=%llu (queue count=%d), trying pre-prepare",
             req->cid, (unsigned long long)req->rid, q->count);

    /* Try to send a pre-prepare if window allows */
    tbft_replica_send_pre_prepare(r);
}

/* --------------------------------------------------------------------------
 * Pre-prepare handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_pre_prepare(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_pre_prepare_rep_t)) {
        ESP_LOGW(TAG, "pp: too short (len=%d)", len);
        return;
    }
    const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)msg;

    /* Reject if wrong view */
    if (pp->view != r->node.view) {
        ESP_LOGW(TAG, "pp: wrong view (got=%lld, expected=%lld)",
                 (long long)pp->view, (long long)r->node.view);
        return;
    }

    /* Must come from current primary */
    int expected_primary = tbft_node_primary(&r->node, pp->view);
    if (r->node.node_id == expected_primary) return; /* primary ignores own PP */

    /* Check sequence number is in window */
    if (!tbft_replica_in_window(r, pp->seqno)) {
        ESP_LOGW(TAG, "pp seqno %lld out of window (last_stable=%lld)",
                 (long long)pp->seqno, (long long)r->last_stable);
        return;
    }

    /* Validate embedded sizes to prevent integer overflow in auth_offset.
     * rset_size and non_det_size must be non-negative and their sum must
     * leave room for the struct header, authenticator, and fit within the
     * received message length. */
    if (pp->rset_size < 0 || pp->non_det_size < 0) {
        ESP_LOGW(TAG, "pp: negative embedded size rset=%d ndet=%d",
                 (int)pp->rset_size, (int)pp->non_det_size);
        return;
    }
    if ((int64_t)sizeof(*pp) + pp->rset_size + pp->non_det_size +
        (int64_t)sizeof(tbft_auth_t) > len) {
        ESP_LOGW(TAG, "pp: embedded sizes exceed message length (need=%lld, have=%d)",
                 (long long)(sizeof(*pp) + pp->rset_size + pp->non_det_size + sizeof(tbft_auth_t)), len);
        return;
    }

    /* Verify authenticator from the primary */
    int auth_offset = (int)(sizeof(*pp) + pp->rset_size + pp->non_det_size);
    if (len < auth_offset + (int)sizeof(tbft_auth_t)) {
        ESP_LOGW(TAG, "pp: missing authenticator");
        return;
    }
    {
        int slot = tbft_node_auth_slot_index(&r->node, expected_primary);
        if (slot < 0) {
            ESP_LOGW(TAG, "pp: invalid auth slot for primary %d", expected_primary);
            return;
        }
        const tbft_auth_t *auth =
            (const tbft_auth_t *)((const uint8_t *)msg + auth_offset);
        if (!tbft_node_verify_auth(&r->node, expected_primary, msg,
                                   (size_t)auth_offset,
                                   &auth->slots[slot],
                                   pp->hdr.timestamp_us)) {
            ESP_LOGW(TAG, "pp: MAC verification failed from primary %d",
                     expected_primary);
            return;
        }
    }

    /* Reject empty request sets.  A zero rset_size would bypass the
     * digest and client-signature re-verification below, allowing a
     * Byzantine primary to inject a pre-prepare with no actual request. */
    if (pp->rset_size == 0) {
        ESP_LOGW(TAG, "pp: empty request set (rset_size=0) rejected");
        return;
    }

    /* Verify request set digest */
    if (pp->rset_size > 0) {
        const uint8_t *req_set = (const uint8_t *)msg + sizeof(*pp);
        tbft_digest_t computed;
        tbft_msg_digest(req_set, (size_t)pp->rset_size, &computed);
        if (!tbft_digest_equal(&pp->digest, &computed)) {
            ESP_LOGW(TAG, "pp: request set digest mismatch");
            return;
        }

        /* Re-verify the embedded client RSA signature.  Without this, a
         * Byzantine primary could inject forged requests attributed to
         * arbitrary clients — backups would accept the PP (digest matches,
         * HMAC from primary is valid), prepare, commit, and execute an
         * operation the client never signed.  PBFT safety assumes client
         * authorisation is cryptographically tied to each request; do NOT
         * trust the primary's claim that the embedded bytes are well-formed. */
        if ((size_t)pp->rset_size < sizeof(tbft_request_rep_t) + sizeof(tbft_sig_t)) {
            ESP_LOGW(TAG, "pp: embedded request too short (%d)",
                     (int)pp->rset_size);
            return;
        }
        const tbft_request_rep_t *ereq =
            (const tbft_request_rep_t *)req_set;
        if (ereq->hdr.tag != TBFT_MSG_REQUEST) {
            ESP_LOGW(TAG, "pp: embedded message is not a Request (tag=%d)",
                     (int)ereq->hdr.tag);
            return;
        }
        if (ereq->command_size < 0 ||
            (size_t)ereq->command_size >
                (size_t)pp->rset_size - sizeof(tbft_request_rep_t) - sizeof(tbft_sig_t)) {
            ESP_LOGW(TAG, "pp: embedded command_size %d doesn't fit in rset_size %d",
                     (int)ereq->command_size, (int)pp->rset_size);
            return;
        }
        if (ereq->cid < 0 || ereq->cid >= r->node.num_principals) {
            ESP_LOGW(TAG, "pp: embedded invalid cid %d", (int)ereq->cid);
            return;
        }
        size_t sig_off = sizeof(tbft_request_rep_t) + (size_t)ereq->command_size;
        /* The embedded request must fit exactly: hdr+rep+cmd+sig == rset_size.
         * Anything larger means the primary packed untrusted trailing data. */
        if (sig_off + sizeof(tbft_sig_t) != (size_t)pp->rset_size) {
            ESP_LOGW(TAG, "pp: embedded request size mismatch (sig_off=%zu, rset=%d)",
                     sig_off, (int)pp->rset_size);
            return;
        }
        const tbft_sig_t *ereq_sig =
            (const tbft_sig_t *)(req_set + sig_off);
        if (!tbft_node_verify_sig(&r->node, (tbft_node_id_t)ereq->cid,
                                  ereq, sig_off, ereq_sig)) {
            ESP_LOGW(TAG, "pp: embedded request signature invalid from cid=%d",
                     (int)ereq->cid);
            return;
        }
    }

    /* Store in agreement region */
    if (!tbft_ar_store_pp(&r->ar, pp->seqno, msg, len)) {
        ESP_LOGW(TAG, "pp: failed to store in agreement region seqno=%lld",
                 (long long)pp->seqno);
        return;
    }

    ESP_LOGI(TAG, "pp accepted: seqno=%lld view=%lld from primary %d",
             (long long)pp->seqno, (long long)pp->view, expected_primary);

    /* Update last_prepared if needed */
    if (pp->seqno > r->last_prepared) {
        r->last_prepared = pp->seqno;
    }

    /* Send prepare */
    tbft_replica_send_prepare(r, pp->seqno);
}

/* --------------------------------------------------------------------------
 * Prepare handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_prepare(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_prepare_rep_t) + (int)sizeof(tbft_auth_t)) {
        ESP_LOGW(TAG, "prepare: too short (len=%d)", len);
        return;
    }
    const tbft_prepare_rep_t *prep = (const tbft_prepare_rep_t *)msg;

    if (prep->view != r->node.view) {
        ESP_LOGW(TAG, "prepare: wrong view (got=%lld, expected=%lld)",
                 (long long)prep->view, (long long)r->node.view);
        return;
    }
    if (!tbft_replica_in_window(r, prep->seqno)) {
        ESP_LOGW(TAG, "prepare: seqno %lld out of window", (long long)prep->seqno);
        return;
    }

    tbft_node_id_t sender = prep->id;
    if (sender < 0 || sender >= r->node.num_replicas) {
        ESP_LOGW(TAG, "prepare: invalid sender id %d", sender);
        return;
    }
    if (sender == tbft_node_primary(&r->node, prep->view)) return;
    if (sender == r->node.node_id) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) {
        ESP_LOGW(TAG, "prepare: invalid auth slot for sender %d", sender);
        return;
    }

    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*prep));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*prep),
                               &auth->slots[slot], hdr->timestamp_us)) {
        ESP_LOGW(TAG, "prepare: MAC verification failed from %d", sender);
        return;
    }

    /* If we already have the corresponding Pre_prepare, require the Prepare's
     * digest to match it — otherwise this Prepare is either for a different
     * request (Byzantine) or we are the one with a tampered PP.  Reject here
     * before storing so the certificate's f+1 value slots are not poisoned
     * with bogus digests. */
    int pp_len = 0;
    const uint8_t *pp_bytes = tbft_ar_load_pp(&r->ar, prep->seqno, &pp_len);
    if (pp_bytes && pp_len >= (int)sizeof(tbft_pre_prepare_rep_t)) {
        const tbft_pre_prepare_rep_t *pp =
            (const tbft_pre_prepare_rep_t *)pp_bytes;
        if (!tbft_digest_equal(&pp->digest, &prep->digest)) {
            ESP_LOGW(TAG, "prepare: digest mismatch from %d seqno=%lld",
                     sender, (long long)prep->seqno);
            return;
        }
    }

    /* HIGH FIX H2: Verify Pre_prepare exists BEFORE storing prepare in the
     * agreement region.  If we stored first and then returned on missing PP,
     * the prepare would remain in the AR, polluting certificate slots with
     * junk for seqnos that have no pre-prepare. */
    if (!pp_bytes) {
        ESP_LOGW(TAG, "prepare: no pre-prepare for seqno=%lld",
                 (long long)prep->seqno);
        return;
    }

    bool accepted = tbft_ar_add_prepare(&r->ar, prep->seqno, msg, len, sender);
    if (!accepted) {
        ESP_LOGW(TAG, "prepare: rejected by agreement region seqno=%lld",
                 (long long)prep->seqno);
        return;
    }

    ESP_LOGI(TAG, "prepare accepted: seqno=%lld from replica %d",
             (long long)prep->seqno, sender);

    /* Check if prepared — if so, send commit */
    if (tbft_ar_prepared(&r->ar, prep->seqno)) {
        ESP_LOGI(TAG, "prepared seqno=%lld, sending commit",
                 (long long)prep->seqno);
        tbft_replica_send_commit(r, prep->seqno);
    }
}

/* --------------------------------------------------------------------------
 * Commit handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_commit(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_commit_rep_t) + (int)sizeof(tbft_auth_t)) {
        ESP_LOGW(TAG, "commit: too short (len=%d)", len);
        return;
    }
    const tbft_commit_rep_t *cm = (const tbft_commit_rep_t *)msg;

    if (cm->view != r->node.view) {
        ESP_LOGW(TAG, "commit: wrong view (got=%lld, expected=%lld)",
                 (long long)cm->view, (long long)r->node.view);
        return;
    }
    if (!tbft_replica_in_window(r, cm->seqno)) {
        ESP_LOGW(TAG, "commit: seqno %lld out of window", (long long)cm->seqno);
        return;
    }

    tbft_node_id_t sender = cm->id;
    if (sender < 0 || sender >= r->node.num_replicas) {
        ESP_LOGW(TAG, "commit: invalid sender id %d", sender);
        return;
    }
    if (sender == r->node.node_id) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) {
        ESP_LOGW(TAG, "commit: invalid auth slot for sender %d", sender);
        return;
    }

    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*cm));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*cm),
                               &auth->slots[slot], hdr->timestamp_us)) {
        ESP_LOGW(TAG, "commit: MAC verification failed from %d", sender);
        return;
    }

    /* If we have the PP, require cm->digest == pp->digest.  PBFT commits must
     * be for the same value as the pre-prepare they follow; without this
     * check, Byzantine replicas can push f+1 distinct-digest commits into
     * the certificate and starve legitimate commits of value slots. */
    {
        int pp_len = 0;
        const uint8_t *pp_bytes = tbft_ar_load_pp(&r->ar, cm->seqno, &pp_len);
        if (!pp_bytes || pp_len < (int)sizeof(tbft_pre_prepare_rep_t)) {
            /* Pre-prepare hasn't arrived yet — defer this commit.
             * A valid commit must reference a pre-prepared value. */
            ESP_LOGW(TAG, "commit: no pre-prepare for seqno=%lld from %d — deferring",
                     (long long)cm->seqno, sender);
            return;
        }
        const tbft_pre_prepare_rep_t *pp =
            (const tbft_pre_prepare_rep_t *)pp_bytes;
        if (!tbft_digest_equal(&pp->digest, &cm->digest)) {
            ESP_LOGW(TAG, "commit: digest mismatch from %d seqno=%lld",
                     sender, (long long)cm->seqno);
            return;
        }
    }

    bool accepted = tbft_ar_add_commit(&r->ar, cm->seqno, msg, len, sender);
    if (!accepted) {
        ESP_LOGW(TAG, "commit: rejected by agreement region seqno=%lld",
                 (long long)cm->seqno);
        return;
    }

    ESP_LOGI(TAG, "commit accepted: seqno=%lld from replica %d",
             (long long)cm->seqno, sender);

    /* Check if committed-local */
    if (tbft_ar_committed(&r->ar, cm->seqno)) {
        ESP_LOGI(TAG, "committed seqno=%lld, executing",
                 (long long)cm->seqno);
        tbft_replica_execute_committed(r);
    }
}

/* --------------------------------------------------------------------------
 * Checkpoint handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_checkpoint(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_checkpoint_rep_t) + (int)sizeof(tbft_auth_t)) return;
    const tbft_checkpoint_rep_t *ckpt = (const tbft_checkpoint_rep_t *)msg;

    /* Enforce seqno alignment to the checkpoint interval.  An unaligned
     * Byzantine Checkpoint (e.g. seqno=3 when interval=10) would otherwise be
     * stored at an arbitrary slot, and with enough conspirators could cause
     * mark_stable() to truncate the agreement region at a non-checkpoint
     * boundary. */
    if (ckpt->seqno <= 0 || (ckpt->seqno % TBFT_CHECKPOINT_INTERVAL) != 0) {
        ESP_LOGW(TAG, "checkpoint: unaligned seqno %lld (interval=%d)",
                 (long long)ckpt->seqno, (int)TBFT_CHECKPOINT_INTERVAL);
        return;
    }

    /* Never trust ckpt->id from the wire as proof of identity.
     * Our own checkpoint is stored directly in execute_committed at send time,
     * so any network message claiming to be from us is ignored here. */
    tbft_node_id_t sender = ckpt->id;
    if (sender < 0 || sender >= r->node.num_replicas) return;
    if (sender == r->node.node_id) return; /* already stored at send time */

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) return;

    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*ckpt));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*ckpt),
                               &auth->slots[slot], hdr->timestamp_us)) {
        ESP_LOGW(TAG, "checkpoint: MAC verification failed from %d", sender);
        return;
    }

    bool stable = tbft_cr_store(&r->cr, ckpt->seqno, sender, msg, len);

    if (stable) {
        ESP_LOGI(TAG, "stable checkpoint at seqno=%lld",
                 (long long)ckpt->seqno);
        tbft_replica_mark_stable(r, ckpt->seqno);
    }
}

/* --------------------------------------------------------------------------
 * View-change handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_view_change(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_view_change_rep_t)) return;
    const tbft_view_change_rep_t *vc = (const tbft_view_change_rep_t *)msg;

    /* Validate sender id before using it */
    tbft_node_id_t sender_id = (tbft_node_id_t)vc->id;
    if (sender_id < 0 || sender_id >= r->node.num_replicas) return;

    /* Verify RSA signature.  hdr.size is the body length (without the sig).
     * The sender appends the sig after the body but does not include it in
     * hdr.size, so the total wire size is hdr.size + TBFT_SIG_SIZE. */
    int32_t body_size = vc->hdr.size;
    if (body_size < (int32_t)sizeof(tbft_view_change_rep_t) || body_size > len) {
        ESP_LOGW(TAG, "view-change: invalid body_size %d", (int)body_size);
        return;
    }
    if (len < (int)(body_size + (int)sizeof(tbft_sig_t))) {
        ESP_LOGW(TAG, "view-change: missing RSA signature from %d", sender_id);
        return;
    }
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + body_size);
    if (!tbft_node_verify_sig(&r->node, sender_id, msg, (size_t)body_size, sig)) {
        ESP_LOGW(TAG, "view-change: RSA signature verification failed from %d",
                 sender_id);
        return;
    }

    bool collected = tbft_vi_collect_vc(&r->vi, sender_id, msg, len);
    if (!collected) {
        if (vc->v > r->node.view) {
            /* CRITICAL FIX: Advance view and reset seqno immediately when
             * receiving a view-change for a higher view.  Without this,
             * replicas can accumulate different view numbers from idling
             * (vc timers fire at different times), each keeping their own
             * stale seqno.  By advancing view + resetting seqno as soon
             * as we learn about a higher view, all replicas self-synchronize
             * without requiring simultaneous restart. */
            r->node.view = vc->v;
            r->node.cur_primary = tbft_node_primary(&r->node, vc->v);
            r->seqno = 1;
            ESP_LOGI(TAG, "sync view to %lld (from vc of %d), seqno reset to 1",
                     (long long)vc->v, sender_id);
            if (!r->vi.received[r->node.node_id]) {
                /* Suppress catch-up view-change until we have enough HMAC keys */
                int keys_ok = 0;
                for (int i = 0; i < r->node.num_replicas; i++) {
                    if (i == r->node.node_id) continue;
                    if (r->node.principals[i] && r->node.principals[i]->keys_fresh) {
                        keys_ok++;
                    }
                }
                if (keys_ok >= r->node.threshold - 1) {
                    tbft_replica_send_view_change(r);
                }
            }
        }
        return;
    }

    /* C1 FIX: Check quorum after EVERY successful VC collection, including
     * when tbft_vi_collect_vc advanced target_view to a higher value.
     * Previously this was inside an `if (!collect_vc)` block, so if a VC
     * for a higher view caused target_view to reset+advance, the quorum
     * check was skipped and the new primary would never send New_view. */
    int new_primary = tbft_node_primary(&r->node, r->vi.target_view);
    if (new_primary == r->node.node_id && tbft_vi_has_quorum(&r->vi)) {
        /* Build and send New_view */
        tbft_seqno_t min_s, max_s;
        tbft_vi_compute_min_max(&r->vi, &min_s, &max_s);

        /* CRITICAL FIX: Include prepared certificate proofs in New_view.
         * PBFT requires the new primary to prove which requests were prepared
         * in the previous view. We collect prepared request info from all
         * 2f+1 view-changes and find seqnos with 2f+1 matching digests. */
        typedef struct {
            tbft_seqno_t seqno;
            tbft_digest_t digest;
            int count;
        } prep_count_t;

        prep_count_t counts[TBFT_WINDOW_SIZE];
        int n_counts = 0;

        for (int i = 0; i < r->node.num_replicas; i++) {
            if (!r->vi.received[i]) continue;
            int vc_len = 0;
            const uint8_t *vc_msg = tbft_sr_load_vc(&r->sr, i, &vc_len);
            if (!vc_msg) continue;
            const tbft_view_change_rep_t *vc_rep =
                (const tbft_view_change_rep_t *)vc_msg;

            /* Validate that the claimed arrays fit within the actual message
             * length.  A Byzantine replica can inflate n_ckpts/n_reqs beyond
             * the stored message size, causing ptr to advance past valid data
             * and read garbage (or OOB) as req_info entries. */
            int ckpts_bytes = vc_rep->n_ckpts * (int)sizeof(tbft_vc_ckpt_t);
            int reqs_bytes  = vc_rep->n_reqs  * (int)sizeof(tbft_vc_req_info_t);
            int header_sz     = (int)sizeof(tbft_view_change_rep_t);
            if (vc_rep->n_ckpts < 0 || vc_rep->n_reqs < 0) continue;
            if (ckpts_bytes < 0 || reqs_bytes < 0) continue; /* overflow guard */
            if (vc_len < header_sz + ckpts_bytes + reqs_bytes) continue;

            /* Parse embedded req_info array */
            const uint8_t *ptr = vc_msg + header_sz;
            if (vc_rep->n_ckpts > 0) {
                ptr += ckpts_bytes;
            }
            const tbft_vc_req_info_t *reqs =
                (const tbft_vc_req_info_t *)ptr;

            for (int j = 0; j < vc_rep->n_reqs; j++) {
                if (reqs[j].seqno < min_s || reqs[j].seqno > max_s) continue;

                int k;
                for (k = 0; k < n_counts; k++) {
                    if (counts[k].seqno == reqs[j].seqno &&
                        tbft_digest_equal(&counts[k].digest, &reqs[j].digest)) {
                        counts[k].count++;
                        break;
                    }
                }
                if (k == n_counts && n_counts < TBFT_WINDOW_SIZE) {
                    counts[n_counts].seqno = reqs[j].seqno;
                    counts[n_counts].digest = reqs[j].digest;
                    counts[n_counts].count = 1;
                    n_counts++;
                }
            }
        }

        /* Build New_view message */
        tbft_new_view_rep_t *nv = (tbft_new_view_rep_t *)r->out_buf;
        nv->hdr.tag          = TBFT_MSG_NEW_VIEW;
        nv->hdr.extra        = 0;
        nv->hdr.timestamp_us = esp_timer_get_time();
        nv->v       = r->vi.target_view;  /* C1 FIX: use target_view, not vc->v */
        nv->min     = min_s;
        nv->max     = max_s;
        nv->has_sig = 1;

        uint8_t *body_ptr = r->out_buf + sizeof(*nv);
        int n_proofs = 0;

        /* Append prepared proofs with quorum */
        for (int k = 0; k < n_counts; k++) {
            if (counts[k].count >= r->node.threshold) {
                tbft_vc_req_info_t *proof = (tbft_vc_req_info_t *)body_ptr;
                proof->seqno = counts[k].seqno;
                proof->last_view = r->node.view > 0 ? (r->node.view - 1) : 0;
                proof->digest = counts[k].digest;
                body_ptr += sizeof(tbft_vc_req_info_t);
                n_proofs++;
            }
        }
        nv->n_prep = n_proofs;

        int32_t body_size = tbft_msg_align((int32_t)(body_ptr - r->out_buf));
        nv->hdr.size = body_size;

        if ((size_t)body_size + sizeof(tbft_sig_t) > sizeof(r->out_buf)) {
            ESP_LOGE(TAG, "new-view: out_buf too small for signature");
            return;
        }
        tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + body_size);
        if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)body_size, sig) != 0) {
            ESP_LOGE(TAG, "new-view: failed to sign; refusing to send");
            return;
        }

        int32_t total_size = body_size + (int32_t)sizeof(tbft_sig_t);
        tbft_node_send(&r->node, r->out_buf, (size_t)total_size,
                       TBFT_ALL_REPLICAS);
        ESP_LOGI(TAG, "sent new-view v=%lld min=%lld max=%lld n_prep=%d",
                 (long long)r->vi.target_view, (long long)min_s, (long long)max_s, n_proofs);
    }
}

/* --------------------------------------------------------------------------
 * New-view handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_new_view(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_new_view_rep_t)) return;
    const tbft_new_view_rep_t *nv = (const tbft_new_view_rep_t *)msg;

    /* Verify it's for the view we expect */
    if (nv->v <= r->node.view) return;

    if (!tbft_vi_verify_nv(&r->vi, nv, len)) {
        ESP_LOGW(TAG, "new-view verification failed for v=%lld",
                 (long long)nv->v);
        return;
    }

    /* Signature is mandatory — an unsigned or malformed New_view would let
     * any attacker drive our view and primary.  The signature covers the
     * full body (hdr.size bytes, including any prepared-entries) so an
     * attacker cannot mutate the trailing payload without invalidating it.
     * hdr.size is the body length; the sig is appended but NOT included in
     * hdr.size, so total wire = hdr.size + TBFT_SIG_SIZE. */
    if (!nv->has_sig) {
        ESP_LOGW(TAG, "new-view: unsigned message rejected (v=%lld)",
                 (long long)nv->v);
        return;
    }

    int32_t body_size = nv->hdr.size;
    if (body_size < (int32_t)sizeof(tbft_new_view_rep_t) || body_size > len) {
        ESP_LOGW(TAG, "new-view: invalid body_size=%d len=%d",
                 (int)body_size, len);
        return;
    }
    if (len < body_size + (int)sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "new-view: missing signature (body=%d len=%d)",
                 (int)body_size, len);
        return;
    }
    const tbft_sig_t *sig =
        (const tbft_sig_t *)((const uint8_t *)msg + body_size);
    int new_primary = tbft_node_primary(&r->node, nv->v);
    if (!tbft_node_verify_sig(&r->node, new_primary,
                              msg, (size_t)body_size, sig)) {
        ESP_LOGW(TAG, "new-view: signature verification failed (v=%lld primary=%d)",
                 (long long)nv->v, new_primary);
        return;
    }

    /* Validate n_prep bounds before processing prepared proofs. */
    if (nv->n_prep < 0 || nv->n_prep > TBFT_WINDOW_SIZE) {
        ESP_LOGW(TAG, "new-view: invalid n_prep=%d", nv->n_prep);
        return;
    }

    /* Install new view */
    r->node.view        = nv->v;
    r->node.cur_primary = tbft_node_primary(&r->node, nv->v);

    /* CRITICAL FIX: Reset seqno for ALL replicas, not just the new primary.
     * During idle view-changes (no requests processed), seqno can drift
     * past the window (e.g., seqno=9, last_stable=0, window=8). If only
     * the primary resets seqno, non-primary replicas retain stale seqno
     * values, causing out-of-window errors when they later become primary. */
    r->seqno = nv->min + 1;
    if (tbft_replica_is_primary(r)) {
        ESP_LOGI(TAG, "new primary: seqno reset to %lld", (long long)r->seqno);
    } else {
        ESP_LOGD(TAG, "replica %d: seqno reset to %lld for view %lld",
                 r->node.node_id, (long long)r->seqno, (long long)nv->v);
    }

    /* Process prepared proofs from New_view to recover unexecuted requests.
     * PBFT requires the new primary to prove which requests were prepared
     * in the previous view. We log the proofs for observability; the new
     * primary will re-send pre-prepares for unexecuted seqnos via normal
     * operation once it starts. */
    if (nv->n_prep > 0) {
        const uint8_t *proofs = (const uint8_t *)msg + sizeof(*nv);
        int n_prep = nv->n_prep;
        size_t proofs_size = (size_t)n_prep * sizeof(tbft_vc_req_info_t);
        if ((size_t)len < sizeof(*nv) + proofs_size) {
            ESP_LOGW(TAG, "new-view: message too short for n_prep=%d (len=%d)", n_prep, len);
            return;
        }
        for (int p = 0; p < n_prep; p++) {
            const tbft_vc_req_info_t *proof =
                (const tbft_vc_req_info_t *)(proofs + p * sizeof(tbft_vc_req_info_t));
            if (proof->seqno > r->last_executed &&
                proof->seqno <= nv->max) {
                ESP_LOGI(TAG, "new-view: prepared seqno=%lld digest=[...%02x] from view %lld",
                         (long long)proof->seqno, proof->digest.bytes[0],
                         (long long)proof->last_view);
            }
        }
    }

    /* Stop view-change timer and restart it */
    tbft_itimer_stop(&r->vtimer);
    tbft_vi_reset(&r->vi, nv->v + 1);

    tbft_itimer_start(&r->vtimer, r->vtimer_period_us);

    ESP_LOGI(TAG, "installed new view %lld, primary=%d",
             (long long)nv->v, r->node.cur_primary);
}

/* --------------------------------------------------------------------------
 * Fetch handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_fetch(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_fetch_rep_t)) return;
    const tbft_fetch_rep_t *fetch = (const tbft_fetch_rep_t *)msg;

    if (fetch->id < 0 || fetch->id >= r->node.num_replicas) {
        ESP_LOGW(TAG, "fetch: invalid sender id %d", fetch->id);
        return;
    }

    /* Respond with Meta_data for the requested level/index */
    int level = fetch->level;
    int index = fetch->index;

    /* Validate level and index as non-negative before using them for array
     * indexing.  A negative value from a malicious peer would bypass the
     * upper-bound checks (e.g. -1 >= p_levels is false, but the negative
     * value would then be used as an array index). */
    if (level < 0 || index < 0) {
        ESP_LOGW(TAG, "fetch: negative level=%d or index=%d", level, index);
        return;
    }

    if (level >= r->state.ptree.dims.p_levels) return;

    if (level == r->state.ptree.dims.p_levels - 1) {
        /* Send TBFT_MSG_DATA containing the requested block */
        if (index >= r->state.num_blocks) return;

        tbft_data_rep_t *data_rep = (tbft_data_rep_t *)r->out_buf;
        data_rep->hdr.tag     = TBFT_MSG_DATA;
        data_rep->hdr.extra   = 0;
        data_rep->seqno       = fetch->c;
        data_rep->block_index = index;
        data_rep->digest      = r->state.ptree.ptree[level][index].digest;

        uint8_t *payload = r->out_buf + sizeof(*data_rep);
        memcpy(payload, r->state.mem + (size_t)index * TBFT_BLOCK_SIZE, TBFT_BLOCK_SIZE);

        int32_t body_size = (int32_t)(sizeof(*data_rep) + TBFT_BLOCK_SIZE);
        data_rep->hdr.size = tbft_msg_align(body_size);

        tbft_node_send(&r->node, r->out_buf, (size_t)data_rep->hdr.size, fetch->id);
        return;
    }

    int pchildren = r->state.ptree.dims.p_children;
    int n_children = tbft_ptree_nodes_at_level(level + 1, pchildren);
    int first_child = index * pchildren;
    int count = pchildren;
    if (first_child + count > n_children) {
        count = n_children - first_child;
    }
    if (count <= 0) return;

    /* Build Meta_data response */
    uint8_t *out = r->out_buf;
    tbft_meta_data_rep_t *md = (tbft_meta_data_rep_t *)out;
    md->hdr.tag  = TBFT_MSG_META_DATA;
    md->hdr.extra = 0;
    md->seqno    = fetch->c;
    md->level    = level;
    md->index    = index;
    md->n_parts  = count;

    tbft_part_info_t *parts =
        (tbft_part_info_t *)(out + sizeof(tbft_meta_data_rep_t));
    for (int c = 0; c < count; c++) {
        int cidx = first_child + c;
        parts[c].digest  = r->state.ptree.ptree[level + 1][cidx].digest;
        parts[c].version = (int32_t)r->state.ptree.ptree[level + 1][cidx].version;
    }

    int32_t total = (int32_t)(sizeof(*md) + (size_t)count * sizeof(tbft_part_info_t));
    md->hdr.size = tbft_msg_align(total);

    tbft_node_send(&r->node, out, (size_t)md->hdr.size, fetch->id);
}

/* --------------------------------------------------------------------------
 * Meta_data / Data handlers (for fetching replica)
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_meta_data(tbft_replica_t *r, const void *msg, int len)
{
    if (!tbft_state_in_fetch(&r->state)) return;
    if (len < (int)sizeof(tbft_meta_data_rep_t)) return;

    const tbft_meta_data_rep_t *md = (const tbft_meta_data_rep_t *)msg;

    if (md->n_parts < 0 || md->n_parts > TBFT_P_CHILDREN) {
        ESP_LOGW(TAG, "meta_data: invalid n_parts=%d", md->n_parts);
        return;
    }
    if (len < (int)(sizeof(tbft_meta_data_rep_t) + (size_t)md->n_parts * sizeof(tbft_part_info_t))) {
        ESP_LOGW(TAG, "meta_data: truncated parts array (n_parts=%d, len=%d)", md->n_parts, len);
        return;
    }

    const tbft_part_info_t *parts =
        (const tbft_part_info_t *)((const uint8_t *)msg + sizeof(*md));

    tbft_state_handle_meta_data(&r->state, md, parts, md->n_parts);

    /* Send next fetch requests */
    int level, index;
    while (tbft_state_next_fetch_req(&r->state, &level, &index)) {
        tbft_fetch_rep_t *fr = (tbft_fetch_rep_t *)r->out_buf;
        fr->hdr.tag   = TBFT_MSG_FETCH;
        fr->hdr.extra = 0;
        fr->last_stable = r->last_stable;
        fr->c         = r->state.fetch_seqno;
        fr->level     = level;
        fr->index     = index;
        fr->id        = r->node.node_id;
        fr->hdr.size  = tbft_msg_align((int32_t)sizeof(*fr));
        tbft_node_send(&r->node, r->out_buf, (size_t)fr->hdr.size,
                       r->state.fetch_replier);
    }
}

void tbft_replica_handle_data(tbft_replica_t *r, const void *msg, int len)
{
    if (!tbft_state_in_fetch(&r->state)) return;
    if (len < (int)sizeof(tbft_data_rep_t) + TBFT_BLOCK_SIZE) return;

    const tbft_data_rep_t *data_rep = (const tbft_data_rep_t *)msg;
    const uint8_t *block_data =
        (const uint8_t *)msg + sizeof(tbft_data_rep_t);

    tbft_state_handle_data(&r->state, data_rep, block_data);
}

/* --------------------------------------------------------------------------
 * Protocol actions
 * -------------------------------------------------------------------------- */

void tbft_replica_send_pre_prepare(tbft_replica_t *r)
{
    if (!tbft_replica_is_primary(r)) return;

    /* CRITICAL FIX: Ensure enough peers have our HMAC out-key before sending
     * pre-prepares.  Without this, the primary sends pre-prepares that
     * backups reject with MAC verification failure because they haven't
     * received the primary's key yet. */
    {
        int keys_ok = 0;
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            if (r->node.principals[i] && r->node.principals[i]->keys_fresh)
                keys_ok++;
        }
        if (keys_ok < r->node.threshold - 1) return;
    }

    /* CRITICAL FIX: Wait for key exchange to settle before sending pre-prepares.
     * The primary's keys_fresh means it received a peer's New_key, but the
     * peer might not yet have received the primary's New_key (network asymmetry).
     * Waiting 3s (>= one full 2s re-broadcast cycle) ensures peers have time
     * to receive and decrypt the primary's key before pre-prepares arrive. */
    {
        TickType_t now = xTaskGetTickCount();
        if (r->last_key_exchange_tick != 0 &&
            now - r->last_key_exchange_tick < pdMS_TO_TICKS(3000)) return;
    }

    /* Track which queue the request came from so we pop from the right one
     * after successfully building and broadcasting the Pre_prepare. */
    tbft_rqueue_t *src_queue = &r->rqueue;
    tbft_rqueue_entry_t *req = rqueue_front(src_queue);
    if (!req) {
        src_queue = &r->ro_rqueue;
        req = rqueue_front(src_queue);
    }
    if (!req) return;

    /* CRITICAL FIX: Dedup by request ID.  The same request can arrive
     * multiple times: once from the client's broadcast, then forwarded
     * by each non-primary replica.  Without dedup here, the primary
     * would assign a different seqno to each duplicate, rapidly draining
     * the sequence window and preventing consensus. */
    {
        const tbft_request_rep_t *rr = (const tbft_request_rep_t *)req->buf;
        if (rr->rid == r->last_assigned_rid) {
            /* Already assigned a seqno for this request — drop duplicate. */
            rqueue_pop(src_queue);
            /* Check the other queue too */
            tbft_rqueue_entry_t *next_req = rqueue_front(src_queue);
            if (!next_req) {
                src_queue = (src_queue == &r->rqueue) ? &r->ro_rqueue : &r->rqueue;
                next_req = rqueue_front(src_queue);
            }
            if (!next_req) return;
            src_queue = (src_queue == &r->rqueue) ? &r->ro_rqueue : &r->rqueue;
            const tbft_request_rep_t *rr2 = (const tbft_request_rep_t *)next_req->buf;
            if (rr2->rid == r->last_assigned_rid) return;  /* all dups */
            req = next_req;
        }
    }

    /* Check window */
    if (!tbft_replica_in_window(r, r->seqno)) {
        ESP_LOGW(TAG, "pre-prepare: seqno=%lld out of window (last_stable=%lld, window=%d)",
                 (long long)r->seqno, (long long)r->last_stable, TBFT_WINDOW_SIZE);
        return;
    }

    /* Compute request set digest */
    tbft_digest_t rset_digest;
    tbft_msg_digest(req->buf, (size_t)req->len, &rset_digest);

    /* Compute non-deterministic choices */
    int ndet_len = 0;
    if (r->comp_ndet_cb) {
        r->comp_ndet_cb(r->seqno, r->ndet_buf, &ndet_len, r->ndet_max_len);
    }

    /* MEDIUM FIX M3: Bounds check before int16_t truncation */
    if (ndet_len > INT16_MAX) {
        ESP_LOGE(TAG, "pre-prepare: ndet_len %d exceeds INT16_MAX", ndet_len);
        ndet_len = INT16_MAX;
    }

    /* Guard against buffer overflow before building the message */
    size_t needed = sizeof(tbft_pre_prepare_rep_t) + (size_t)req->len
                  + (size_t)ndet_len + sizeof(tbft_auth_t);
    size_t aligned_needed = (needed + 7u) & ~7u;
    if (aligned_needed > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "pre-prepare: message too large (%zu > %zu)", aligned_needed, sizeof(r->out_buf));
        return;
    }

    /* MEDIUM FIX M1: Clear out_buf before message construction to prevent
     * stale data from leaking into authenticator computation or padding. */
    memset(r->out_buf, 0, sizeof(r->out_buf));

    /* Build Pre_prepare message */
    tbft_pre_prepare_rep_t *pp = (tbft_pre_prepare_rep_t *)r->out_buf;
    pp->hdr.tag        = TBFT_MSG_PRE_PREPARE;
    pp->hdr.extra      = 0;
    pp->hdr.timestamp_us = esp_timer_get_time();
    pp->view         = r->node.view;
    pp->seqno        = r->seqno;
    pp->digest       = rset_digest;
    pp->rset_size    = req->len;
    pp->non_det_size = (int16_t)ndet_len;

    /* Append request set, non-det choices, then authenticator */
    uint8_t *ptr = r->out_buf + sizeof(*pp);
    memcpy(ptr, req->buf, (size_t)req->len);
    ptr += req->len;
    memcpy(ptr, r->ndet_buf, (size_t)ndet_len);
    ptr += ndet_len;

    /* Authenticator */
    tbft_auth_t *auth = (tbft_auth_t *)ptr;
    int32_t msg_len_before_auth = (int32_t)(ptr - r->out_buf);
    tbft_node_gen_auth(&r->node, r->out_buf, (size_t)msg_len_before_auth,
                       auth);
    ptr += sizeof(tbft_auth_t);

    pp->hdr.size = tbft_msg_align((int32_t)(ptr - r->out_buf));

    /* Store in agreement region */
    tbft_ar_store_pp(&r->ar, r->seqno, r->out_buf, pp->hdr.size);

    /* Broadcast */
    tbft_node_send(&r->node, r->out_buf, (size_t)pp->hdr.size,
                   TBFT_ALL_REPLICAS);

    ESP_LOGI(TAG, "sent pre-prepare seqno=%lld view=%lld (rset=%d, ndet=%d, total=%d)",
             (long long)r->seqno, (long long)r->node.view,
             req->len, ndet_len, pp->hdr.size);

    /* Mark this request ID as assigned to prevent duplicate seqno. */
    {
        const tbft_request_rep_t *rr_final = (const tbft_request_rep_t *)req->buf;
        r->last_assigned_rid = rr_final->rid;
    }

    rqueue_pop(src_queue);
    r->seqno++;

    /* Reset view-change timer — primary is active */
    tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
}

void tbft_replica_send_prepare(tbft_replica_t *r, tbft_seqno_t n)
{
    /* Primary does not send Prepare */
    if (tbft_replica_is_primary(r)) return;

    /* Get the stored pre-prepare digest */
    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
    if (!pp_buf) return;
    const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)pp_buf;

    tbft_prepare_rep_t *prep = (tbft_prepare_rep_t *)r->out_buf;
    prep->hdr.tag   = TBFT_MSG_PREPARE;
    prep->hdr.extra = 0;
    prep->hdr.timestamp_us = esp_timer_get_time();
    prep->view      = r->node.view;
    prep->seqno     = n;
    prep->digest    = pp->digest;
    prep->id        = r->node.node_id;

    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*prep));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*prep), auth);
    prep->hdr.size = tbft_msg_align(
        (int32_t)(sizeof(*prep) + sizeof(tbft_auth_t)));

    /* Store own prepare */
    tbft_ar_add_my_prepare(&r->ar, n, r->out_buf, prep->hdr.size,
                           r->node.node_id);

    tbft_node_send(&r->node, r->out_buf, (size_t)prep->hdr.size,
                   TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent prepare seqno=%lld", (long long)n);
}

void tbft_replica_send_commit(tbft_replica_t *r, tbft_seqno_t n)
{
    /* Get the digest from the stored pre-prepare */
    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
    if (!pp_buf) return;
    const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)pp_buf;

    tbft_commit_rep_t *cm = (tbft_commit_rep_t *)r->out_buf;
    cm->hdr.tag   = TBFT_MSG_COMMIT;
    cm->hdr.extra = 0;
    cm->hdr.timestamp_us = esp_timer_get_time();
    cm->view      = r->node.view;
    cm->seqno     = n;
    cm->digest    = pp->digest;  /* PBFT: commit must include the digest */
    cm->id        = r->node.node_id;

    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*cm));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*cm), auth);
    cm->hdr.size = tbft_msg_align(
        (int32_t)(sizeof(*cm) + sizeof(tbft_auth_t)));

    tbft_ar_add_my_commit(&r->ar, n, r->out_buf, cm->hdr.size,
                          r->node.node_id);

    tbft_node_send(&r->node, r->out_buf, (size_t)cm->hdr.size,
                   TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent commit seqno=%lld", (long long)n);
}

void tbft_replica_execute_committed(tbft_replica_t *r)
{
    /* Execute in sequence-number order.
     * Loop bound: <= last_prepared (not +1) to avoid executing a slot that
     * has not yet been prepared. */
    for (tbft_seqno_t n = r->last_executed + 1;
         n <= r->last_prepared;
         n++) {
        if (!tbft_ar_committed(&r->ar, n)) break;

        int pp_len = 0;
        const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
        if (!pp_buf) break;

        const tbft_pre_prepare_rep_t *pp =
            (const tbft_pre_prepare_rep_t *)pp_buf;
        const uint8_t *req_bytes =
            (const uint8_t *)pp_buf + sizeof(*pp);
        int req_len   = pp->rset_size;
        int ndet_len  = pp->non_det_size;
        const uint8_t *ndet = req_bytes + req_len;

        /* Execute */
        if (r->exec_cb && req_len >= (int)sizeof(tbft_request_rep_t)) {
            const tbft_request_rep_t *req_rep =
                (const tbft_request_rep_t *)req_bytes;

            /* Use r->out_buf for reply; max reply payload fits inside */
            tbft_reply_rep_t *reply = (tbft_reply_rep_t *)r->out_buf;
            uint8_t *rep_payload    = r->out_buf + sizeof(*reply);
            int      rep_len        = 0;
            bool     ro = (req_rep->hdr.extra & TBFT_REQUEST_RO_FLAG) != 0;

            /* Ensure enough room: hdr + actual payload + sig */
            if (sizeof(*reply) + (size_t)rep_len + TBFT_SIG_SIZE > sizeof(r->out_buf)) {
                ESP_LOGE(TAG, "reply too large for out_buf (%d + %d + %d > %zu)",
                         (int)sizeof(*reply), rep_len, (int)TBFT_SIG_SIZE, sizeof(r->out_buf));
                break;
            }

            int rc = r->exec_cb(req_bytes, req_len,
                                rep_payload, &rep_len,
                                (void *)ndet, ndet_len,
                                req_rep->cid, ro);

            if (rc == 0) {
                reply->hdr.tag    = TBFT_MSG_REPLY;
                reply->hdr.extra  = 0;
                reply->view       = r->node.view;
                reply->seqno      = n;
                reply->cid        = req_rep->cid;
                reply->rid        = req_rep->rid;
                reply->reply_size = rep_len;

                int32_t body_sz = (int32_t)(sizeof(*reply) + (size_t)rep_len);
                reply->hdr.size = tbft_msg_align(body_sz);

                /* Sign the reply so the client can verify authenticity */
                tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + reply->hdr.size);
                int32_t total = reply->hdr.size;
                if (reply->hdr.size + (int32_t)sizeof(tbft_sig_t)
                        <= (int32_t)sizeof(r->out_buf)) {
                    if (tbft_node_gen_sig(&r->node, r->out_buf,
                                          (size_t)reply->hdr.size, sig) == 0) {
                        total = reply->hdr.size + (int32_t)sizeof(tbft_sig_t);
                    } else {
                        ESP_LOGE(TAG, "reply signing failed for seqno=%lld — dropping reply",
                                 (long long)n);
                        break;
                    }
                } else {
                    ESP_LOGE(TAG, "reply exceeds buffer after signing — dropping");
                    break;
                }

                ESP_LOGI(TAG, "sending reply: cid=%d rid=%llu seqno=%lld size=%d",
                         req_rep->cid, (unsigned long long)req_rep->rid,
                         (long long)n, total);

                if (r->recv_reply_cb) {
                    r->recv_reply_cb(r->out_buf, total, req_rep->cid);
                }
                tbft_node_send(&r->node, r->out_buf, (size_t)total,
                               req_rep->cid);
            } else {
                /* A failed exec_cb leaves the application state unchanged,
                 * so advancing last_executed here would cause our checkpoint
                 * digest to diverge from honest peers (they applied the op,
                 * we didn't).  Halt execution at this seqno; the retry /
                 * view-change machinery will eventually handle it. */
                ESP_LOGE(TAG, "exec_cb failed for seqno=%lld rc=%d; halting "
                         "execution to preserve checkpoint consistency",
                         (long long)n, rc);
                break;
            }
        }

        r->last_executed = n;

        /* Checkpoint at intervals */
        if ((n % TBFT_CHECKPOINT_INTERVAL) == 0) {
            tbft_state_checkpoint(&r->state, n);

            /* Build and broadcast Checkpoint */
            tbft_checkpoint_rep_t *ckpt =
                (tbft_checkpoint_rep_t *)r->out_buf;
            ckpt->hdr.tag  = TBFT_MSG_CHECKPOINT;
            ckpt->hdr.extra = 0;
            ckpt->hdr.timestamp_us = esp_timer_get_time();
            ckpt->seqno    = n;
            ckpt->digest   = *tbft_state_root_digest(&r->state);
            ckpt->id       = r->node.node_id;
            tbft_auth_t *auth =
                (tbft_auth_t *)(r->out_buf + sizeof(*ckpt));
            tbft_node_gen_auth(&r->node, r->out_buf,
                               sizeof(*ckpt), auth);
            ckpt->hdr.size = tbft_msg_align(
                (int32_t)(sizeof(*ckpt) + sizeof(tbft_auth_t)));

            /* Store our own checkpoint NOW, before broadcast.
             * This prevents a spoofed message with id==node_id from
             * bypassing auth in handle_checkpoint (which ignores self). */
            bool self_stable = tbft_cr_store(&r->cr, n, r->node.node_id,
                                             r->out_buf, ckpt->hdr.size);

            tbft_node_send(&r->node, r->out_buf,
                           (size_t)ckpt->hdr.size, TBFT_ALL_REPLICAS);

            if (self_stable) {
                tbft_replica_mark_stable(r, n);
            }

            ESP_LOGI(TAG, "broadcast checkpoint at seqno=%lld",
                     (long long)n);
        }
    }
}

void tbft_replica_mark_stable(tbft_replica_t *r, tbft_seqno_t seqno)
{
    if (seqno <= r->last_stable) return;

    const tbft_digest_t *winning = tbft_cr_winning_digest(&r->cr, seqno);
    if (winning) {
        const tbft_digest_t *local = tbft_state_root_digest(&r->state);
        if (!tbft_digest_equal(winning, local)) {
            ESP_LOGW(TAG, "stable ckpt digest mismatch at seqno=%lld — starting fetch",
                     (long long)seqno);
            tbft_state_start_fetch(&r->state, seqno,
                                   tbft_node_primary(&r->node, r->node.view));
            return;
        }
    }

    tbft_state_mark_stable(&r->state, seqno);
    r->last_stable = seqno;

    /* Truncate static regions */
    tbft_ar_truncate(&r->ar, seqno + 1);
    tbft_cr_truncate(&r->cr, seqno);

    /* Reset view-change timer — stable progress */
    tbft_itimer_start(&r->vtimer, r->vtimer_period_us);

    ESP_LOGI(TAG, "marked stable seqno=%lld", (long long)seqno);
}

void tbft_replica_send_view_change(tbft_replica_t *r)
{
    tbft_view_t target = r->node.view + 1;
    if (r->vi.target_view > target) {
        target = r->vi.target_view;
    }

    if (r->vi.target_view != target) {
        tbft_vi_reset(&r->vi, target);
    }

    uint8_t *ptr = r->out_buf;

    /* Ensure the fixed-size header + optional ckpt fits before writing */
    size_t min_size = sizeof(tbft_view_change_rep_t) + sizeof(tbft_vc_ckpt_t) + sizeof(tbft_sig_t);
    if (min_size > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "send_view_change: buffer too small");
        return;
    }

    tbft_view_change_rep_t *vc = (tbft_view_change_rep_t *)ptr;
    vc->hdr.tag   = TBFT_MSG_VIEW_CHANGE;
    vc->hdr.extra = 0;
    vc->v         = target;
    vc->ls        = r->last_stable;
    vc->id        = r->node.node_id;

    ptr += sizeof(*vc);

    vc->n_ckpts = 0;
    vc->n_reqs  = 0;

    if (r->last_stable > 0) {
        const tbft_digest_t *root = tbft_state_root_digest(&r->state);
        tbft_vc_ckpt_t *ckpt = (tbft_vc_ckpt_t *)ptr;
        ckpt->seqno  = r->last_stable;
        ckpt->digest = *root;
        ptr += sizeof(*ckpt);
        vc->n_ckpts = 1;
    }

    /* Guard the max_reqs subtraction: if ptr has already advanced past the
     * space budget, a plain (size_t)-(size_t) underflows to a huge unsigned
     * value and the loop would iterate unbounded. */
    size_t consumed_bytes = (size_t)(ptr - r->out_buf);
    size_t budget_bytes   = sizeof(r->out_buf) - sizeof(tbft_sig_t);
    int max_reqs = 0;
    if (consumed_bytes < budget_bytes) {
        max_reqs = (int)((budget_bytes - consumed_bytes) /
                         sizeof(tbft_vc_req_info_t));
    }
    for (tbft_seqno_t s = r->last_stable + 1; s <= r->last_prepared; s++) {
        if (tbft_ar_in_range(&r->ar, s) && tbft_ar_prepared(&r->ar, s)) {
            if (vc->n_reqs >= TBFT_WINDOW_SIZE || vc->n_reqs >= max_reqs) break;
            int pp_len = 0;
            const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, s, &pp_len);
            if (pp_buf) {
                const tbft_pre_prepare_rep_t *pp =
                    (const tbft_pre_prepare_rep_t *)pp_buf;
                tbft_vc_req_info_t *req = (tbft_vc_req_info_t *)ptr;
                req->seqno     = s;
                req->last_view = r->node.view;
                req->digest    = pp->digest;
                ptr += sizeof(*req);
                vc->n_reqs++;
            }
        }
    }

    vc->hdr.size = tbft_msg_align((int32_t)(ptr - r->out_buf));

    /* Append RSA signature over the body (hdr.size bytes).
     * The body length is stored in hdr.size; the signature follows but is
     * NOT included in hdr.size so receivers can locate it as msg + hdr.size.
     *
     * SECURITY: View-change messages MUST be signed.  An unsigned view-change
     * could be injected by any network attacker to trigger a view transition.
     * If signing fails, abort — do not fall back to sending unsigned. */
    int32_t body_size  = vc->hdr.size;
    int32_t total_size = body_size + (int32_t)sizeof(tbft_sig_t);
    if (total_size > (int32_t)sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "send_view_change: out_buf too small for signature");
        return;
    }
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + body_size);
    if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)body_size, sig) != 0) {
        ESP_LOGE(TAG, "send_view_change: failed to sign — refusing to send unsigned");
        return;
    }

    tbft_node_send(&r->node, r->out_buf, (size_t)total_size, TBFT_ALL_REPLICAS);

    ESP_LOGI(TAG, "sent view-change to view %lld", (long long)target);

    if (!r->vi.received[r->node.node_id]) {
        tbft_vi_collect_vc(&r->vi, r->node.node_id, r->out_buf, total_size);
    }

    /* Low #7 FIX: This sets a fixed 2x grace period for the view-change
     * retry timer (not true exponential backoff). The period resets to
     * the base value on each send_view_change call. */
    tbft_itimer_start(&r->vtimer, r->vtimer_period_us * 2);
}

/* --------------------------------------------------------------------------
 * Session key exchange (New_key)
 * -------------------------------------------------------------------------- */

void tbft_replica_send_new_key(tbft_replica_t *r)
{
    int n        = r->node.num_replicas;
    int local_id = r->node.node_id;

    /* Max message size check: header + (n-1) slots */
    size_t max_needed = sizeof(tbft_new_key_rep_t)
                      + (size_t)(n - 1) * sizeof(tbft_new_key_slot_t);
    if (max_needed > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "send_new_key: out_buf too small (%zu needed, %zu available)",
                 max_needed, sizeof(r->out_buf));
        return;
    }

    tbft_new_key_rep_t *nk = (tbft_new_key_rep_t *)r->out_buf;
    nk->hdr.tag   = TBFT_MSG_NEW_KEY;
    nk->hdr.extra = 0;
    nk->id        = local_id;
    nk->n_keys    = 0;

    tbft_new_key_slot_t *slots =
        (tbft_new_key_slot_t *)(r->out_buf + sizeof(*nk));
    int slot_idx = 0;

    for (int i = 0; i < n; i++) {
        if (i == local_id) continue;

        tbft_principal_t *p = r->node.principals[i];
        if (!p) continue;

        /* Generate a cryptographically random session key */
        tbft_hmac_key_t new_key;
        esp_fill_random(new_key.bytes, sizeof(new_key.bytes));

        /* Store locally as the out-key for messages we send TO replica i */
        tbft_principal_set_out_key(p, &new_key);

        /* Encrypt the key under replica i's RSA public key */
        size_t enc_len = 0;
        int rc = tbft_principal_encrypt_new_key(p, &new_key,
                                                 slots[slot_idx].ciphertext,
                                                 sizeof(slots[slot_idx].ciphertext),
                                                 &enc_len);
        if (rc != 0 || enc_len != TBFT_SIG_SIZE) {
            ESP_LOGW(TAG, "send_new_key: encrypt for replica %d failed", i);
            /* M3 FIX: Skip this slot entirely — don't zero or include it.
             * The slot_idx is NOT incremented, so this slot is excluded
             * from the final message. Zeroing it would leave garbage
             * recipient_id=0 which could confuse the recipient. */
            continue;
        }

        slots[slot_idx].recipient_id = i;
        slot_idx++;
    }

    if (slot_idx == 0) return;

    nk->n_keys = slot_idx;
    int32_t body = (int32_t)(sizeof(*nk)
                               + (size_t)slot_idx * sizeof(tbft_new_key_slot_t));
    nk->hdr.size = tbft_msg_align(body);

    /* CRITICAL FIX: RSA-sign New_key messages to prevent authentication bypass.
     * Without this, any network participant can inject arbitrary HMAC keys. */
    int32_t total = nk->hdr.size + (int32_t)sizeof(tbft_sig_t);
    if ((size_t)total > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "send_new_key: out_buf too small for signature");
        return;
    }
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + nk->hdr.size);
    if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)nk->hdr.size, sig) != 0) {
        ESP_LOGE(TAG, "send_new_key: failed to sign; refusing to send");
        return;
    }

    tbft_node_send(&r->node, r->out_buf, (size_t)total,
                   TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent New_key with %d slots (signed)", slot_idx);
}

void tbft_replica_handle_new_key(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_new_key_rep_t)) return;
    const tbft_new_key_rep_t *nk = (const tbft_new_key_rep_t *)msg;

    tbft_node_id_t sender_id = (tbft_node_id_t)nk->id;
    if (sender_id < 0 || sender_id >= r->node.num_replicas) return;
    if (sender_id == r->node.node_id) return; /* ignore own broadcasts */

    if (nk->n_keys <= 0 || nk->n_keys > r->node.num_replicas) return;

    /* CRITICAL FIX: Verify RSA signature on New_key messages.
     * Without this, any network participant can inject arbitrary HMAC keys,
     * completely bypassing the authentication system. */
    int32_t body_size = nk->hdr.size;
    if (body_size < (int32_t)sizeof(tbft_new_key_rep_t) || body_size > len) {
        ESP_LOGW(TAG, "new_key from %d: invalid body_size %d", sender_id, (int)body_size);
        return;
    }
    if (len < body_size + (int)sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "new_key from %d: missing RSA signature", sender_id);
        return;
    }
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + body_size);
    if (!tbft_node_verify_sig(&r->node, sender_id, msg, (size_t)body_size, sig)) {
        ESP_LOGW(TAG, "new_key from %d: RSA signature verification failed", sender_id);
        return;
    }

    int expected_len = (int)(sizeof(*nk)
                             + (size_t)nk->n_keys * sizeof(tbft_new_key_slot_t));
    if (len < expected_len) {
        ESP_LOGW(TAG, "new_key from %d: message too short", sender_id);
        return;
    }

    const tbft_new_key_slot_t *slots =
        (const tbft_new_key_slot_t *)((const uint8_t *)msg + sizeof(*nk));

    for (int i = 0; i < nk->n_keys; i++) {
        if (slots[i].recipient_id != r->node.node_id) continue;

        /* This slot is addressed to us — decrypt with our RSA private key */
        tbft_principal_t *local = r->node.local_principal;
        if (!local) return;

        tbft_hmac_key_t new_key;
        int rc = tbft_principal_decrypt_new_key(local,
                                                 slots[i].ciphertext,
                                                 TBFT_SIG_SIZE,
                                                 &new_key);
        if (rc != 0) {
            ESP_LOGW(TAG, "new_key: decrypt from replica %d failed", sender_id);
            return;
        }

        /* Install as the in-key for verifying messages FROM sender_id */
        tbft_principal_t *p = r->node.principals[sender_id];
        if (p) {
            tbft_principal_set_in_key(p, &new_key);
            r->last_key_exchange_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "installed HMAC in-key from replica %d", sender_id);
        }

        /* Explicit wipe of plaintext key after installation */
        memset(&new_key, 0, sizeof(new_key));
        return; /* only one slot per sender per message */
    }
}
