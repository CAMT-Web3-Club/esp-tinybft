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
#define TBFT_VC_BACKOFF_MAX_MULT  60   /* max multiplier for view-change timeout backoff */

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

static void rqueue_clear(tbft_rqueue_t *q) {
    memset(q->entries, 0, sizeof(q->entries));
    q->head  = 0;
    q->tail  = 0;
    q->count = 0;
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

static void tbft_replica_reset_forwarded_requests(tbft_replica_t *r)
{
    for (int i = 0; i < r->node.num_principals; i++) {
        r->last_forwarded_rid[i] = r->last_executed_rid[i];
    }
    /* Clear the request queue so stale entries from prior views
     * don't block fresh client requests.  Without this, a queue
     * full of orphaned-forwards prevents new requests from
     * reaching the front, producing infinite client timeouts. */
    rqueue_clear(&r->rqueue);
    rqueue_clear(&r->ro_rqueue);
}

bool tbft_replica_has_pending_requests(const tbft_replica_t *r)
{
    if (r->vi.in_progress) {
        return true;
    }
    if (tbft_replica_is_primary(r)) {
        for (int i = 0; i < r->node.num_principals; i++) {
            if (r->last_received_rid[i] > r->last_executed_rid[i]) {
                return true;
            }
        }
    } else {
        for (int i = 0; i < r->node.num_principals; i++) {
            if (r->last_forwarded_rid[i] > r->last_executed_rid[i]) {
                return true;
            }
        }
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

int tbft_replica_init(tbft_replica_t *r,
                      tbft_node_id_t node_id, int f, int num_replicas, int num_nodes,
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
    if (tbft_node_init(&r->node, node_id, f, num_replicas, num_nodes,
                       mcast_ip, auth_timeout_us, port) != 0) {
        return -1;
    }

    int n = num_replicas;
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
    r->last_ckpt_throttle_us    = 0;
    r->last_fetch_throttle_us    = 0;
    r->last_prepared            = 0;
    r->last_executed            = 0;
    r->last_tentative_execute   = 0;
    r->pending_fill_seqno       = 0;
    r->fill_started_at_us       = 0;

    /* Timer periods (defaults — configurable via menuconfig, overridden by
     * Byz_init_replica from config file). */
    r->vtimer_period_us = TBFT_VIEW_CHANGE_TIMEOUT_US;
    r->stimer_period_us = TBFT_STATUS_TIMEOUT_US;

    /* Init timers (not started — caller must set periods and start).
     * NOTE: rtimer (recovery) and ntimer (keep-alive) are reserved for
     * future use. They are not initialised or freed to avoid allocating
     * unused esp_timer handles. */
    if (tbft_itimer_init(&r->vtimer, vtimer_cb, r, "tbft_vtimer") != ESP_OK ||
        tbft_itimer_init(&r->stimer, stimer_cb, r, "tbft_stimer") != ESP_OK) {
        ESP_LOGE(TAG, "replica: timer init failed");
        tbft_itimer_free(&r->vtimer);
        tbft_itimer_free(&r->stimer);
        tbft_state_free(&r->state);
        tbft_node_free(&r->node);
        return -1;
    }

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

static void handle_status(tbft_replica_t *r, const void *msg, int len);
static void tbft_replica_start_fetch(tbft_replica_t *r, tbft_seqno_t seqno, int replier);

static void tbft_replica_start_fetch(tbft_replica_t *r, tbft_seqno_t seqno, int replier)
{
    tbft_state_start_fetch(&r->state, seqno, replier);

    /* Send initial fetch request(s) */
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

void tbft_replica_run(tbft_replica_t *r)
{
    r->running = true;
    ESP_LOGI(TAG, "replica %d starting event loop", r->node.node_id);

    /* Stagger initial New_key broadcast by node_id to reduce simultaneous
     * fragmented UDP storms (New_key = 1840 bytes > 1500 MTU). */
    vTaskDelay(pdMS_TO_TICKS(r->node.node_id * 300));
    tbft_replica_send_new_key(r);

    /* Time-bounded key barrier: wait up to 60s for at least 2*f remote keys.
     * To achieve a quorum of 2f+1 (including ourselves), we only need 2f remote
     * keys ready. Requiring all n-1 keys would completely break f-fault tolerance
     * if even a single node is offline or fails to complete key exchange. */
    {
        const int needed = 2 * r->node.max_faulty;
        TickType_t key_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        TickType_t last_nk = xTaskGetTickCount();
        ESP_LOGI(TAG, "waiting for at least %d HMAC keys to start consensus (timeout=60s)", needed);
        while (xTaskGetTickCount() < key_deadline) {
            int keys_ok = 0;
            for (int i = 0; i < r->node.num_replicas; i++) {
                if (i == r->node.node_id) continue;
                if (r->node.principals[i] && r->node.principals[i]->keys_fresh)
                    keys_ok++;
            }
            if (keys_ok >= needed) {
                ESP_LOGI(TAG, "at least %d HMAC keys ready (%d total), breaking barrier early", needed, keys_ok);
                break;
            }
            /* Drain any available NEW_KEY messages (batch of up to 16) */
            for (int d = 0; d < 16; d++) {
                tbft_node_id_t src_id = -1;
                int n = tbft_node_recv(&r->node, r->node.recv_buf,
                                       sizeof(r->node.recv_buf), &src_id);
                if (n < (int)sizeof(tbft_msg_hdr_t)) break;
                const tbft_msg_hdr_t *hdr =
                    (const tbft_msg_hdr_t *)r->node.recv_buf;
                if (n >= hdr->size && hdr->tag == TBFT_MSG_NEW_KEY)
                    tbft_replica_handle_new_key(r, r->node.recv_buf, n);
            }
            /* Re-broadcast our own New_key every 3s */
            if (xTaskGetTickCount() - last_nk >= pdMS_TO_TICKS(3000)) {
                tbft_replica_send_new_key(r);
                last_nk = xTaskGetTickCount();
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        int keys_ok = 0;
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            if (r->node.principals[i] && r->node.principals[i]->keys_fresh)
                keys_ok++;
        }
        ESP_LOGI(TAG, "key barrier complete: %d/%d keys ready",
                 keys_ok, r->node.num_replicas - 1);
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
                ? pdMS_TO_TICKS(60000) : pdMS_TO_TICKS(5000);
            TickType_t now = xTaskGetTickCount();
            if (now - last_nk_send >= interval) {
                tbft_replica_send_new_key(r);
                last_nk_send = now;
            }
        }

        if (r->evt_group) {
            EventBits_t bits = xEventGroupWaitBits(
                r->evt_group,
                TBFT_EVT_VTIMER | TBFT_EVT_STIMER,
                pdTRUE,   /* clear on exit */
                pdFALSE,  /* wait for any bit */
                pdMS_TO_TICKS(10));
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
                    ESP_LOGD(TAG, "view-change suppressed: keys=%d/%d — re-arming",
                             keys_ok, r->node.threshold - 1);
                    /* Re-arm the one-shot timer if there are still pending requests. */
                    if (tbft_replica_has_pending_requests(r)) {
                        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
                    } else {
                        tbft_itimer_stop(&r->vtimer);
                    }
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
            tbft_replica_start_fetch(r, r->state.fetch_seqno, new_replier);
        }

        /* Send pending fill request, rate-limited per-seqno.
         * Set by tbft_replica_execute_committed when it hits an uncommitted
         * or PP-missing seqno (the gap-stall bug). Cleared here once the
         * seqno is committed/executed or leaves the window.
         *
         * Two-layer escalation (v0.2.14):
         *   1. (0–10s)  fill to current primary, 500ms throttle  (v0.2.13)
         *   2. (>10s)    abandon seqno: last_executed := pending_fill_seqno
         *                — cluster unblocks; client must retransmit.
         *
         * Rationale: in the most common gap-stall case (current primary
         * has the stored PP), Layer 1 closes the gap within 500ms. In the
         * rarer "no replica has the PP" case (e.g., original primary
         * crashed mid-broadcast, view changes brought primaries that
         * also lack the PP), Layer 1 will keep failing forever. Layer 2
         * is the last-resort unblocker: the local replica abandons the
         * stuck seqno, advancing last_executed past it. Bounded state
         * divergence (≤ f+1 replicas) is recovered on the next checkpoint
         * via the existing state-fetch protocol. The client detects
         * TBFT_CLIENT_REPLY_TIMEOUT_MS and retransmits.
         *
         * A cross-view broadcast-fill (sending fill_request to all
         * replicas instead of just the primary) was considered but does
         * not help: the PP stored in any backup's ar slot is for the
         * original view, and the requester (in a later view) rejects
         * PPs from earlier views in handle_pre_prepare. Layer 2 abandon
         * is the correct recovery for the cross-view PP-loss case.
         *
         * v0.2.15 addition: Layer 1 short-circuits when the PP is
         * already stored in the agreement region slice.  In that case
         * the gap is about missing commit certificates, not the PP
         * itself — re-sending fill requests would hit
         * `tbft_prepared_cert_add_pp`'s "slot occupied" check every
         * 500ms and waste bandwidth, and the abandon log message would
         * misleadingly say "no replica had the PP" when one clearly
         * did (the local replica, which stored it on the first
         * reception).  Skipping the redundant request lets the backup
         * wait passively for the missing commits to arrive.
         */
        if (r->pending_fill_seqno > 0) {
            if (r->pending_fill_seqno > r->last_executed
                && tbft_replica_in_window(r, r->pending_fill_seqno)) {
                tbft_agreement_slice_t *fsl =
                    tbft_ar_slice(&r->ar, r->pending_fill_seqno);
                int64_t now_us = esp_timer_get_time();
                int64_t elapsed_us = (r->fill_started_at_us > 0)
                    ? (now_us - r->fill_started_at_us) : 0;

                /* Check whether the PP is already in the slice.  If so,
                 * the gap is about missing commit certificates, not the
                 * PP itself; skip Layer 1's redundant fill request and
                 * rely on commits arriving to close the gap. */
                int pp_len_check = 0;
                const uint8_t *pp_already =
                    tbft_ar_load_pp(&r->ar, r->pending_fill_seqno,
                                    &pp_len_check);
                bool pp_stored = (pp_already != NULL && pp_len_check > 0);

                /* Layer 1: fill to current primary, 500ms throttle */
                if (!pp_stored
                    && now_us - fsl->fill_sent_us >= 500000LL) {
                    fsl->fill_sent_us = now_us;

                    tbft_fill_request_rep_t *fr =
                        (tbft_fill_request_rep_t *)r->out_buf;
                    memset(fr, 0, sizeof(*fr));
                    fr->hdr.tag          = TBFT_MSG_FILL_REQUEST;
                    fr->hdr.extra        = 0;
                    fr->hdr.size         =
                        tbft_msg_align((int32_t)sizeof(*fr));
                    fr->hdr.timestamp_us = now_us;
                    fr->view             = r->node.view;
                    fr->seqno            = r->pending_fill_seqno;
                    fr->id               = r->node.node_id;

                    /* HMAC authenticate the request for the primary.
                     * Uses the primary's out-key (which equals our
                     * session in-key from the primary, by New_key
                     * handshake symmetry). */
                    int primary =
                        tbft_node_primary(&r->node, r->node.view);
                    tbft_principal_t *pri = r->node.principals[primary];
                    if (pri) {
                        int32_t body_len =
                            (int32_t)(sizeof(*fr) - sizeof(tbft_mac_t));
                        tbft_principal_gen_mac_out(pri,
                            r->out_buf, (size_t)body_len, &fr->mac);
                    }

                    tbft_node_send(&r->node, r->out_buf,
                                   (size_t)fr->hdr.size, primary);
                    ESP_LOGI(TAG,
                        "fill: requested pre-prepare for seqno=%lld from primary %d",
                        (long long)r->pending_fill_seqno, primary);
                }

                /* Layer 1.5: when the PP is stored and we have reached
                 * prepare quorum but not commit quorum, periodically
                 * re-broadcast our commit to help close the gap.  Each
                 * re-send may trigger a chain reaction through other
                 * replicas (handle_commit receives it and re-sends their
                 * own commit).  Rate-limited to 500ms, matching Layer 1
                 * and the handle_commit cooldown. */
                if (pp_stored
                    && tbft_ar_prepared(&r->ar, r->pending_fill_seqno)
                    && !tbft_ar_committed(&r->ar, r->pending_fill_seqno)
                    && now_us - fsl->commit_sent_us >= 500000LL) {
                    tbft_replica_send_commit(r, r->pending_fill_seqno);
                }

                /* Layer 2: after 10s, abandon the seqno. Force-advance
                 * last_executed past the stuck seqno so subsequent in-window
                 * seqnos can execute. The client's request at this seqno
                 * is effectively a no-op on this replica; the client should
                 * detect timeout (TBFT_CLIENT_REPLY_TIMEOUT_MS) and
                 * retransmit. */
                if (elapsed_us >= 10 * 1000 * 1000LL) {
                    if (pp_stored) {
                        ESP_LOGE(TAG,
                            "fill: abandoning stuck seqno=%lld after %lldms —"
                            " PP stored but commit quorum not reached"
                            " (likely network drop or Byzantine),"
                            " client must retransmit",
                            (long long)r->pending_fill_seqno,
                            (long long)(elapsed_us / 1000));
                    } else {
                        ESP_LOGE(TAG,
                            "fill: abandoning stuck seqno=%lld after %lldms —"
                            " no replica had the PP, client must retransmit",
                            (long long)r->pending_fill_seqno,
                            (long long)(elapsed_us / 1000));
                    }
                    r->last_executed     = r->pending_fill_seqno;
                    r->last_prepared     = r->last_executed;
                    r->pending_fill_seqno  = 0;
                    r->fill_started_at_us  = 0;
                }
            } else {
                /* Gap closed (executed) or seqno left the window — clear */
                r->pending_fill_seqno  = 0;
                r->fill_started_at_us  = 0;
            }
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
            vTaskDelay(pdMS_TO_TICKS(10));
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

        /* Until we have the sender's HMAC session key, only process non-HMAC messages.
         * View_change and New_view use RSA signatures, so they can be processed immediately.
         * Pre_prepare, Prepare, Commit, and Checkpoint use HMAC authenticators, so we must
         * have the sender's session key before we can verify and accept them. This prevents
         * both silent verification failure drops and state corruption. */
        if (hdr->tag == TBFT_MSG_PRE_PREPARE) {
            const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)r->node.recv_buf;
            int primary_id = tbft_node_primary(&r->node, pp->view);
            if (primary_id != r->node.node_id) {
                tbft_principal_t *p = r->node.principals[primary_id];
                if (!p || !p->keys_fresh) {
                    ESP_LOGD(TAG, "dropping PP from primary %d: key not fresh", primary_id);
                    continue;
                }
            }
        } else if (hdr->tag == TBFT_MSG_PREPARE) {
            const tbft_prepare_rep_t *prep = (const tbft_prepare_rep_t *)r->node.recv_buf;
            int sender_id = prep->id;
            if (sender_id >= 0 && sender_id < r->node.num_replicas) {
                tbft_principal_t *p = r->node.principals[sender_id];
                if (!p || !p->keys_fresh) {
                    ESP_LOGD(TAG, "dropping PREP from %d: key not fresh", sender_id);
                    continue;
                }
            }
        } else if (hdr->tag == TBFT_MSG_COMMIT) {
            const tbft_commit_rep_t *cm = (const tbft_commit_rep_t *)r->node.recv_buf;
            int sender_id = cm->id;
            if (sender_id >= 0 && sender_id < r->node.num_replicas) {
                tbft_principal_t *p = r->node.principals[sender_id];
                if (!p || !p->keys_fresh) {
                    ESP_LOGD(TAG, "dropping COMMIT from %d: key not fresh", sender_id);
                    continue;
                }
            }
        } else if (hdr->tag == TBFT_MSG_CHECKPOINT) {
            const tbft_checkpoint_rep_t *ckpt = (const tbft_checkpoint_rep_t *)r->node.recv_buf;
            int sender_id = ckpt->id;
            if (sender_id >= 0 && sender_id < r->node.num_replicas) {
                tbft_principal_t *p = r->node.principals[sender_id];
                if (!p || !p->keys_fresh) {
                    ESP_LOGD(TAG, "dropping CKPT from %d: key not fresh", sender_id);
                    continue;
                }
            }
        } else if (hdr->tag == TBFT_MSG_FILL_REQUEST) {
            /* The primary must have a fresh HMAC key for the requester in
             * order to verify the request. We don't know the requester yet
             * (it's inside the message), so defer the freshness check to
             * tbft_replica_handle_fill_request. */
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
        case TBFT_MSG_STATUS:
            handle_status(r, r->node.recv_buf, n);
            break;
        case TBFT_MSG_FILL_REQUEST:
            tbft_replica_handle_fill_request(r, r->node.recv_buf, n);
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

    if (req->rid > r->last_received_rid[req->cid]) {
        r->last_received_rid[req->cid] = req->rid;
    }

    /* If we are not the primary, forward to the primary only (unicast).
     * The client already broadcasts to all replicas, so the primary likely
     * received the request directly — this forward is a fallback in case
     * the client's direct send to the primary failed.  Unicasting (not
     * broadcasting) prevents the exponential forwarding storm that occurs
     * when every replica re-broadcasts every forwarded request.
     *
     * IMPORTANT: Do NOT restart the view-change timer here.  Restarting
     * on every forwarding creates a livelock: if the client sends faster
     * than the timer period, the timer never expires and the cluster
     * is stuck with a faulty primary forever.  The timer is restarted
     * when actual progress is observed (pre-prepare, checkpoint, etc.). */
    if (!tbft_replica_is_primary(r)) {
        int primary = tbft_node_primary(&r->node, r->node.view);
        ESP_LOGD(TAG, "forwarding request to primary %d (view=%lld)",
                 primary, (long long)r->node.view);
        tbft_node_send(&r->node, msg, (size_t)len, primary);

        if (req->rid > r->last_forwarded_rid[req->cid]) {
            r->last_forwarded_rid[req->cid] = req->rid;
        }

        if (r->last_forwarded_rid[req->cid] > r->last_executed_rid[req->cid]) {
            if (!tbft_itimer_is_running(&r->vtimer)) {
                ESP_LOGI(TAG, "vtimer started on backup: pending request from cid=%d rid=%llu",
                         req->cid, (unsigned long long)req->rid);
                tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
            }
        }
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

    /* Determine expected primary for this view BEFORE any state changes. */
    int expected_primary = tbft_node_primary(&r->node, pp->view);

    /* Reject if wrong view — but note if the pre-prepare is from a
     * higher view (primary is ahead of this replica). */
    if (pp->view != r->node.view) {
        if (pp->view > r->node.view) {
            ESP_LOGI(TAG, "pp: higher view %lld (current=%lld), will advance after auth check",
                     (long long)pp->view, (long long)r->node.view);
        } else {
            ESP_LOGW(TAG, "pp: wrong view (got=%lld, expected=%lld)",
                     (long long)pp->view, (long long)r->node.view);
            return;
        }
    }

    /* Must come from current primary */
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
        ESP_LOGI(TAG, "pp: verifying MAC from primary %d, slot=%d, ts=%lld, auth_offset=%d, msg_len=%d, rset=%d, ndet=%d, hdr[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x",
                 expected_primary, slot, (long long)pp->hdr.timestamp_us, auth_offset, len,
                 (int)pp->rset_size, (int)pp->non_det_size,
                 ((uint8_t*)msg)[0], ((uint8_t*)msg)[1], ((uint8_t*)msg)[2], ((uint8_t*)msg)[3],
                 ((uint8_t*)msg)[4], ((uint8_t*)msg)[5], ((uint8_t*)msg)[6], ((uint8_t*)msg)[7]);
        if (!tbft_node_verify_auth(&r->node, expected_primary, msg,
                                   (size_t)auth_offset,
                                   &auth->slots[slot],
                                   pp->hdr.timestamp_us)) {
            ESP_LOGW(TAG, "pp: MAC verification failed from primary %d",
                     expected_primary);
            return;
        }
    }

    /* Only advance view after successful sender and MAC verification */
    if (pp->view > r->node.view) {
        r->node.view = pp->view;
        r->node.cur_primary = tbft_node_primary(&r->node, r->node.view);
        tbft_replica_reset_forwarded_requests(r);
        if (r->last_prepared < r->last_stable) {
            r->last_prepared = r->last_stable;
        }
        if (r->last_executed < r->last_stable) {
            r->last_executed = r->last_stable;
        }
        tbft_cr_truncate(&r->cr, r->last_stable);
        tbft_ar_init(&r->ar, r->ar.prepare_threshold, r->ar.commit_threshold);
        r->ar.head = r->last_stable + 1;
        r->last_prepared = r->last_executed;
        tbft_itimer_stop(&r->vtimer);
        tbft_vi_reset(&r->vi, r->node.view + 1);
        r->vi.in_progress = false;
        if (tbft_replica_has_pending_requests(r)) {
            tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
        } else {
            tbft_itimer_stop(&r->vtimer);
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
        /* The embedded request must fit within rset_size: hdr+rep+cmd+sig
         * ≤ rset_size.  The client aligns messages to 8 bytes with zero
         * padding, so rset_size may be slightly larger than the exact
         * data size.  Allow up to 7 bytes of alignment padding. */
        if (sig_off + sizeof(tbft_sig_t) > (size_t)pp->rset_size ||
            (size_t)pp->rset_size - sig_off - sizeof(tbft_sig_t) > 7) {
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
        if (ereq->rid > r->last_received_rid[ereq->cid]) {
            r->last_received_rid[ereq->cid] = ereq->rid;
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

    /* Do NOT restart the vtimer on every pre-prepare. The vtimer on backups
     * should only run when the backup has actively forwarded a request to the
     * primary (line 505).  Restarting it on every pre-prepare means a backup
     * that receives a pre-prepare and then sees 5s of idle time will trigger
     * a spurious view-change — even though the primary is working correctly. */

    /* Do NOT update last_prepared here. Backups should only advance last_prepared
     * when they verify a quorum of prepares (completing the Prepared Certificate). */

    /* Keep sequence numbers monotonic with the accepted pre-prepare sequence number.
     * Since the primary has proposed pp->seqno, the next sequence number to propose
     * must be at least pp->seqno + 1. */
    if (r->seqno < pp->seqno + 1) {
        r->seqno = pp->seqno + 1;
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
    /* Accept prepares from all replicas including the primary.
     * The primary broadcasts its prepare after sending pre-prepare,
     * and backups need it to reach the f+1 prepare threshold. */
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

    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, prep->seqno);
    if (tbft_bitmap_test(&sl->prepared_cert.pc.bmap, sender)) {
        ESP_LOGD(TAG, "prepare: duplicate from replica %d seqno=%lld",
                 sender, (long long)prep->seqno);
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

    /* Debug: check prepared state */
    int pp_len2 = 0;
    const uint8_t *pp2 = tbft_ar_load_pp(&r->ar, prep->seqno, &pp_len2);
    bool is_prep = tbft_ar_prepared(&r->ar, prep->seqno);
    ESP_LOGI(TAG, "prepare debug: seqno=%lld has_pp=%d prepared=%d",
             (long long)prep->seqno, pp2 ? 1 : 0, is_prep ? 1 : 0);

    /* Check if prepared — if so, update last_prepared, send commit and check for execution */
    if (is_prep) {
        if (prep->seqno > r->last_prepared) {
            r->last_prepared = prep->seqno;
        }
        if (!tbft_bitmap_test(&sl->commit_cert.bmap, r->node.node_id)) {
            ESP_LOGI(TAG, "prepared seqno=%lld, sending commit",
                     (long long)prep->seqno);
            tbft_replica_send_commit(r, prep->seqno);
        }
        if (tbft_ar_committed(&r->ar, prep->seqno)) {
            ESP_LOGI(TAG, "prepared and committed seqno=%lld, executing",
                     (long long)prep->seqno);
            tbft_replica_execute_committed(r);
        }
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
     * be for the same value as the pre-prepare they follow.
     *
     * If the PP has not arrived yet (network reordering or loss), store the
     * commit now — the certificate's multi-value tracking (f+1 slots for f
     * faulty replicas) ensures Byzantine commits with wrong digests cannot
     * prevent the correct digest from reaching the 2f+1 quorum (pigeonhole
     * principle).  Digest verification is repeated at execution time when
     * the PP is guaranteed to be available. */
    {
        int pp_len = 0;
        const uint8_t *pp_bytes = tbft_ar_load_pp(&r->ar, cm->seqno, &pp_len);
        if (pp_bytes && pp_len >= (int)sizeof(tbft_pre_prepare_rep_t)) {
            const tbft_pre_prepare_rep_t *pp =
                (const tbft_pre_prepare_rep_t *)pp_bytes;
            if (!tbft_digest_equal(&pp->digest, &cm->digest)) {
                ESP_LOGW(TAG, "commit: digest mismatch from %d seqno=%lld",
                         sender, (long long)cm->seqno);
                return;
            }
        }
    }

    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, cm->seqno);
    if (tbft_bitmap_test(&sl->commit_cert.bmap, sender)) {
        ESP_LOGD(TAG, "commit: duplicate from replica %d seqno=%lld",
                 sender, (long long)cm->seqno);
        return;
    }

    bool accepted = tbft_ar_add_commit(&r->ar, cm->seqno, msg, len, sender);
    if (!accepted) {
        ESP_LOGW(TAG, "commit: rejected by agreement region seqno=%lld",
                 (long long)cm->seqno);
        return;
    }

    ESP_LOGI(TAG, "commit accepted: seqno=%lld from replica %d",
             (long long)cm->seqno, sender);

    /* If we are prepared for this seqno, re-broadcast our own commit.
     * Our commit may have been lost (UDP/ESP-NOW are unreliable), and
     * receiving another replica's commit signals the cluster is
     * approaching the committed state — re-sending ours helps close
     * the gap and avoid a view-change timeout.
     *
     * To prevent a massive broadcast storm / infinite loop, we only re-send if:
     * 1. We are prepared, but NOT yet committed-local. (Once committed-local, we do not need to re-send).
     * 2. At least 500ms has elapsed since the last time we sent/re-sent our commit for this seqno.
     */
    if (tbft_ar_prepared(&r->ar, cm->seqno) && !tbft_ar_committed(&r->ar, cm->seqno)) {
        int64_t now = esp_timer_get_time();
        if (now - sl->commit_sent_us >= 500000) { /* 500ms cooldown rate limit */
            tbft_replica_send_commit(r, cm->seqno);
        }
    }

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
    /* The MAC was computed over the checkpoint body only (not the auth),
     * so verify over sizeof(*ckpt), not hdr->size which includes auth. */
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
            /* Advance view when receiving a view-change for a higher view.
             * Do NOT send a catch-up view-change here — that creates a cascade
             * where each node overshoots to V+1, causing others to jump to V+2,
             * etc. Just update our view and let the new primary (if we have
             * enough VCs) send the New_view message. */
            r->node.view = vc->v;
            r->node.cur_primary = tbft_node_primary(&r->node, vc->v);
            tbft_replica_reset_forwarded_requests(r);
            /* Keep sequence numbers contiguous and preserve executed/prepared progress */
            tbft_seqno_t start_seq = r->last_stable;
            if (r->last_prepared > start_seq) start_seq = r->last_prepared;
            if (r->last_executed > start_seq) start_seq = r->last_executed;
            if (r->seqno < start_seq + 1) {
                r->seqno = start_seq + 1;
            }
            if (r->last_prepared < r->last_stable) {
                r->last_prepared = r->last_stable;
            }
            if (r->last_executed < r->last_stable) {
                r->last_executed = r->last_stable;
            }
            tbft_cr_truncate(&r->cr, r->last_stable);
            tbft_ar_init(&r->ar, r->ar.prepare_threshold, r->ar.commit_threshold);
            r->ar.head = r->last_stable + 1;
            r->last_prepared = r->last_executed;
            ESP_LOGI(TAG, "sync view to %lld (from vc of %d), seqno reset to %lld",
                     (long long)vc->v, sender_id, (long long)r->seqno);
            /* REMOVED: catch-up view-change cascade accelerator.
             * The old code here called tbft_replica_send_view_change(r) which
             * computed target = r->node.view + 1 = V + 1, overshooting the
             * received view and triggering a cascade to ever-higher views. */
            /* Restart the vtimer with backoff to give the new primary
             * for this view a fair chance without resetting to 1×. */
            tbft_itimer_stop(&r->vtimer);
            if (tbft_replica_has_pending_requests(r)) {
                int shift = (int)(vc->v - r->node.view);
                if (shift < 1) shift = 1;
                if (shift > 6) shift = 6;
                int64_t period = r->vtimer_period_us * (1LL << shift);
                if (period > r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT) {
                    period = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
                }
                tbft_itimer_start(&r->vtimer, period);
            }
        }
        return;
    }

    if (vc->v > r->vi.target_view) {
        tbft_vi_reset(&r->vi, vc->v);
        tbft_vi_collect_vc(&r->vi, vc->id, msg, len);
        if (vc->v == r->vi.target_view && tbft_vi_has_quorum(&r->vi)) {
            /* Sync vi state so subsequent New_view for this view validates */
            r->vi.target_view = vc->v;
            r->vi.in_progress = false;
            /* Restart the vtimer with backoff to give the new primary
             * for this view a fair chance without resetting to 1×. */
            tbft_itimer_stop(&r->vtimer);
            if (tbft_replica_has_pending_requests(r)) {
                int shift = (int)(vc->v - r->node.view);
                if (shift < 1) shift = 1;
                if (shift > 6) shift = 6;
                int64_t period = r->vtimer_period_us * (1LL << shift);
                if (period > r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT) {
                    period = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
                }
                tbft_itimer_start(&r->vtimer, period);
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

        /* Static to avoid stack overflow with large TBFT_WINDOW_SIZE.
         * Safe: handle_view_change only runs in the single replica event loop task. */
        static prep_count_t counts[TBFT_WINDOW_SIZE];
        int n_counts = 0;
        memset(counts, 0, sizeof(counts));

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
             * and read garbage (or OOB) as req_info entries.
             * Use int64_t for the multiplications to prevent signed overflow
             * before the bounds check (signed overflow with plain int could
             * wrap to a small positive, bypassing the < 0 guard). */
            if (vc_rep->n_ckpts < 0 || vc_rep->n_reqs < 0) continue;
            if (vc_rep->n_ckpts > TBFT_WINDOW_SIZE || vc_rep->n_reqs > TBFT_WINDOW_SIZE) continue;
            int64_t ckpts_bytes = (int64_t)vc_rep->n_ckpts * (int64_t)sizeof(tbft_vc_ckpt_t);
            int64_t reqs_bytes  = (int64_t)vc_rep->n_reqs  * (int64_t)sizeof(tbft_vc_req_info_t);
            int64_t header_sz   = (int64_t)sizeof(tbft_view_change_rep_t);
            if (vc_len < (int)(header_sz + ckpts_bytes + reqs_bytes)) continue;

            /* Parse embedded req_info array */
            const uint8_t *ptr = vc_msg + (ptrdiff_t)header_sz;
            if (vc_rep->n_ckpts > 0) {
                ptr += (ptrdiff_t)ckpts_bytes;
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
        const uint8_t *body_end = r->out_buf + sizeof(r->out_buf) - sizeof(tbft_sig_t);
        int n_proofs = 0;

        /* Append prepared proofs with quorum.
         * Guard each iteration: writing past body_end would overflow out_buf. */
        for (int k = 0; k < n_counts; k++) {
            if (counts[k].count >= r->node.threshold) {
                if (body_ptr + sizeof(tbft_vc_req_info_t) > body_end) {
                    ESP_LOGE(TAG, "new-view: out_buf full, truncating proofs at %d", n_proofs);
                    break;
                }
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

        /* Install the new view locally immediately so we can start acting
         * as primary even before loopback receipt. */
        r->node.view = r->vi.target_view;
        r->node.cur_primary = tbft_node_primary(&r->node, r->node.view);
        tbft_replica_reset_forwarded_requests(r);
        /* Compute a monotonic seqno strictly inside the active window.
         * nv->max = last_stable + WINDOW_SIZE (= upper window bound), so
         * nv->max + 1 is OUTSIDE the window [last_stable+1, last_stable+WINDOW_SIZE].
         * Instead, start from nv->min and advance past any prepared proofs
         * or local progress to maintain linearizability. */
        {
            tbft_seqno_t start_seq = nv->min;
            if (r->last_prepared > start_seq) start_seq = r->last_prepared;
            if (r->last_executed > start_seq) start_seq = r->last_executed;
            if (nv->n_prep > 0) {
                const uint8_t *proofs_ptr = r->out_buf + sizeof(*nv);
                for (int p = 0; p < nv->n_prep; p++) {
                    const tbft_vc_req_info_t *proof =
                        (const tbft_vc_req_info_t *)(proofs_ptr + p * sizeof(tbft_vc_req_info_t));
                    if (proof->seqno > start_seq) start_seq = proof->seqno;
                }
            }
            r->seqno = start_seq + 1;
        }
        if (nv->min > r->last_stable)   r->last_stable   = nv->min;
        tbft_state_mark_stable(&r->state, nv->min);
        if (nv->min > r->last_executed) r->last_executed = nv->min;
        if (nv->min > r->last_prepared) r->last_prepared = nv->min;
        tbft_cr_truncate(&r->cr, nv->min);
        tbft_ar_init(&r->ar, r->ar.prepare_threshold, r->ar.commit_threshold);
        r->ar.head = nv->min + 1;
        r->last_prepared = r->last_executed;
        tbft_itimer_stop(&r->vtimer);
        tbft_vi_reset(&r->vi, r->node.view + 1);
        r->vi.in_progress = false;
        if (tbft_replica_has_pending_requests(r)) {
            tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
        } else {
            tbft_itimer_stop(&r->vtimer);
        }
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
    if (nv->v < r->node.view) return;

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
    tbft_replica_reset_forwarded_requests(r);

    /* Compute a monotonic seqno strictly inside the active window.
     * nv->max = last_stable + WINDOW_SIZE (= upper window bound), so
     * nv->max + 1 is OUTSIDE the window.  Start from nv->min and
     * advance past local progress and prepared proofs. */
    {
        tbft_seqno_t start_seq = nv->min;
        if (r->last_prepared > start_seq) start_seq = r->last_prepared;
        if (r->last_executed > start_seq) start_seq = r->last_executed;
        if (nv->n_prep > 0) {
            const uint8_t *proofs_ptr = (const uint8_t *)msg + sizeof(*nv);
            for (int p = 0; p < nv->n_prep; p++) {
                const tbft_vc_req_info_t *proof =
                    (const tbft_vc_req_info_t *)(proofs_ptr + p * sizeof(tbft_vc_req_info_t));
                if (proof->seqno > start_seq) start_seq = proof->seqno;
            }
        }
        r->seqno = start_seq + 1;
    }

    /* If prepared proofs from prior views pushed start_seq past our
     * last_executed, those seqnos were certified as prepared by f+1
     * replicas.  Advance last_executed so the execution loop doesn't
     * encounter an uncommitted gap and break permanently — the proofs
     * confirm these were handled in a prior view. */
    if (nv->n_prep > 0) {
        const uint8_t *proofs_ptr = (const uint8_t *)msg + sizeof(*nv);
        for (int p = 0; p < nv->n_prep; p++) {
            const tbft_vc_req_info_t *proof =
                (const tbft_vc_req_info_t *)(proofs_ptr + p * sizeof(tbft_vc_req_info_t));
            if (proof->seqno > r->last_executed) {
                ESP_LOGI(TAG, "new-view: advancing last_executed from %lld"
                         " to %lld (prepared proof from view %lld)",
                         (long long)r->last_executed,
                         (long long)proof->seqno,
                         (long long)proof->last_view);
                r->last_executed = proof->seqno;
                if (r->last_prepared < proof->seqno)
                    r->last_prepared = proof->seqno;
            }
        }
    }

    /* Advance local markers to nv->min so tbft_replica_in_window is consistent
     * with ar.head = nv->min + 1.  nv->min is the highest stable checkpoint
     * seqno proven by the view-change quorum, so advancing is safe. */
    if (nv->min > r->last_stable)   r->last_stable   = nv->min;
    tbft_state_mark_stable(&r->state, nv->min);
    if (nv->min > r->last_executed) r->last_executed = nv->min;
    if (nv->min > r->last_prepared) r->last_prepared = nv->min;
    tbft_cr_truncate(&r->cr, nv->min);
    /* Re-init clears all slice bitmaps/hashes from the old view.
     * Patching head after init is safe: all slices are clean. */
    tbft_ar_init(&r->ar, r->ar.prepare_threshold, r->ar.commit_threshold);
    r->ar.head = nv->min + 1;

    /* When nv->min is 0 (no stable checkpoint) but nv->n_prep carries
     * prepared seqnos from a prior view, r->seqno can land beyond the
     * window [nv->min+1, nv->min+1+WINDOW_SIZE).  Advance the head so
     * the primary can propose within range rather than being permanently
     * blocked with "pre-prepare: seqno out of window". */
    if (r->seqno >= r->ar.head + TBFT_WINDOW_SIZE) {
        ESP_LOGI(TAG, "new-view: advancing window head from %lld"
                 " to %lld for seqno coverage",
                 (long long)r->ar.head, (long long)r->seqno);
        tbft_ar_truncate(&r->ar, r->seqno);
        r->last_stable   = r->seqno - 1;
        r->last_executed = r->last_stable;
        r->last_prepared = r->last_stable;
    }
    r->last_prepared = r->last_executed;
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

    /* Stop view-change timer and restart it with backoff.
     * The base-period restart from the old code (vtimer_period_us)
     * defeated the exponential backoff — every new-view install
     * reset the timer to 1×, so send_view_change's doubled period
     * never survived more than one view. */
    tbft_itimer_stop(&r->vtimer);
    tbft_vi_reset(&r->vi, nv->v + 1);
    r->vi.in_progress = false;

    if (tbft_replica_has_pending_requests(r)) {
        int shift = (int)(nv->v - r->node.view);
        if (shift < 1) shift = 1;
        if (shift > 6) shift = 6;
        int64_t period = r->vtimer_period_us * (1LL << shift);
        if (period > r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT) {
            period = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
        }
        tbft_itimer_start(&r->vtimer, period);
    } else {
        tbft_itimer_stop(&r->vtimer);
    }

    ESP_LOGI(TAG, "installed new view %lld, primary=%d",
             (long long)nv->v, r->node.cur_primary);
}

/* --------------------------------------------------------------------------
 * Status handler
 * --------------------------------------------------------------------------
 * NOTE: STATUS messages carry NO authenticator (HMAC or signature).  An on-path
 * attacker can inject spurious status messages to trigger unnecessary state
 * fetches.  The protocol tolerates this — state-fetch targets are verified
 * via Meta_data/Data digest checks so a wrong fetch can't corrupt local
 * state.
 *
 * View advancement (st->view > local view): the replica catches up to the
 * peer's view and restarts the vtimer.  No view-change is sent here — the
 * vtimer will trigger a proper view-change if the new primary fails to
 * make progress, following the normal view-change protocol.
 * -------------------------------------------------------------------------- */

static void handle_status(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_status_rep_t)) return;
    const tbft_status_rep_t *st = (const tbft_status_rep_t *)msg;

    if (st->id < 0 || st->id >= r->node.num_replicas) return;
    if (st->id == r->node.node_id) return;

    if (st->view < r->node.view) {
        ESP_LOGW(TAG, "status from %d: sender view=%lld < local view=%lld",
                 st->id, (long long)st->view, (long long)r->node.view);
        return;
    }

    if (st->view > r->node.view) {
        ESP_LOGI(TAG, "status from %d: peer view=%lld last_stable=%lld last_exec=%lld"
                 " | local view=%lld last_stable=%lld",
                 st->id, (long long)st->view, (long long)st->last_stable,
                 (long long)st->last_executed,
                 (long long)r->node.view, (long long)r->last_stable);
        /* Catch up: advance to the peer's view and restart the vtimer.
         * Do NOT call tbft_replica_send_view_change here — it would
         * propose view+1 (leapfrogging past this view), leaving the
         * replica out of sync with peers that are still at the current
         * view.  Instead, advance the view, reset any in-progress
         * view-change that may have been targeting an older view, and
         * let the normal vtimer trigger a proper view-change if the
         * new primary fails to make progress. */
        r->node.view = st->view;
        r->node.cur_primary = tbft_node_primary(&r->node, st->view);
        tbft_replica_reset_forwarded_requests(r);
        if (r->vi.in_progress) {
            tbft_vi_reset(&r->vi, st->view + 1);
            r->vi.in_progress = false;
        }
        /* Re-initialise agreement and checkpoint regions for the new view.
         * This mirrors the cleanup in handle_pre_prepare (lines 660-662).
         * Without it, stale slice data from the old view could pollute
         * certificate tracking in the new view. */
        tbft_cr_truncate(&r->cr, r->last_stable);
        tbft_ar_init(&r->ar, r->ar.prepare_threshold, r->ar.commit_threshold);
        r->ar.head = r->last_stable + 1;
        r->last_prepared = r->last_executed;

        /* Keep sequence numbers contiguous and preserve executed/prepared progress */
        tbft_seqno_t start_seq = r->last_stable;
        if (r->last_prepared > start_seq) start_seq = r->last_prepared;
        if (r->last_executed > start_seq) start_seq = r->last_executed;
        if (r->seqno < start_seq + 1) {
            r->seqno = start_seq + 1;
        }
        ESP_LOGI(TAG, "status view catch-up to %lld, seqno reset to %lld",
                 (long long)st->view, (long long)r->seqno);

        /* If we're behind in state (e.g. freshly booted into a running
         * cluster), immediately fetch so we can participate in consensus.
         * Without this, a node with last_stable=0 catches up the view
         * but never its state, and the in-order-execution requirement
         * (execute: seqno=1 not committed yet) blocks all new requests. */
        if (st->last_stable > r->last_stable && !r->state.in_fetch) {
            int replier = tbft_node_primary(&r->node, st->view);
            ESP_LOGI(TAG, "view catch-up triggered state fetch to seqno=%lld"
                     " from primary %d", (long long)st->last_stable, replier);
            /* Stagger fetch starts to avoid all replicas fetching at once. */
            r->last_fetch_throttle_us = esp_timer_get_time();
            tbft_replica_start_fetch(r, st->last_stable, replier);
        }

        /* Use exponential backoff so the view-change timer grows with each
         * successive failure.  Without this, Status-based view catch-up
         * resets the timer to 1× base every cycle, and the backoff in
         * send_view_change never takes effect. */
        int view_gap = (int)(st->view - r->node.view);
        int shift = view_gap;
        if (shift < 1) shift = 1;
        if (shift > 6) shift = 6;
        int64_t period = r->vtimer_period_us * (1LL << shift);
        if (period > r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT) {
            period = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
        }
        tbft_itimer_stop(&r->vtimer);
        if (tbft_replica_has_pending_requests(r)) {
            tbft_itimer_start(&r->vtimer, period);
        } else {
            tbft_itimer_stop(&r->vtimer);
        }
    }

    if (st->last_executed > r->last_executed
        && st->last_stable > r->last_stable
        && (!r->state.in_fetch || st->last_stable > r->state.fetch_seqno)
        && st->last_executed - r->last_executed > TBFT_CHECKPOINT_INTERVAL) {
        /* Rate-limit fetch starts to avoid flooding. */
        int64_t now = esp_timer_get_time();
        if (now - r->last_fetch_throttle_us >= 500000LL) {
            r->last_fetch_throttle_us = now;
            tbft_replica_start_fetch(r, st->last_stable, st->id);
        }
    }

    /* Check if peer B (st->id) is behind our execution or stable point.
     * If so, send B our checkpoint messages for all checkpoints B is missing.
     *
     * Throttle: when every peer responds to the same Status broadcast in a
     * burst, sending checkpoints to each one creates amplification.  Rate-
     * limit to one batch per 500 ms to break the storm while still helping
     * lagging replicas catch up within a few Status cycles. */
    {
        int64_t now_us = esp_timer_get_time();
        if (now_us - r->last_ckpt_throttle_us < 500000LL) return;
        r->last_ckpt_throttle_us = now_us;
    }
    tbft_seqno_t start_seq = ((st->last_stable / TBFT_CHECKPOINT_INTERVAL) + 1) * TBFT_CHECKPOINT_INTERVAL;
    for (tbft_seqno_t seqno = start_seq;
         seqno <= r->last_executed;
         seqno += TBFT_CHECKPOINT_INTERVAL) {
        
        tbft_digest_t digest;
        if (tbft_state_get_checkpoint_digest(&r->state, seqno, &digest)) {
            tbft_checkpoint_rep_t *ckpt = (tbft_checkpoint_rep_t *)r->out_buf;
            ckpt->hdr.tag  = TBFT_MSG_CHECKPOINT;
            ckpt->hdr.extra = 0;
            ckpt->hdr.timestamp_us = esp_timer_get_time();
            ckpt->seqno    = seqno;
            ckpt->digest   = digest;
            ckpt->id       = r->node.node_id;
            ckpt->hdr.size = tbft_msg_align((int32_t)(sizeof(*ckpt) + sizeof(tbft_auth_t)));
            
            tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*ckpt));
            tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*ckpt), auth);
            
            tbft_node_send(&r->node, r->out_buf, (size_t)ckpt->hdr.size, st->id);
            ESP_LOGI(TAG, "sent checkpoint for seqno %lld to lagging replica %d",
                     (long long)seqno, st->id);
        }
    }
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

        /* Validate Data message fits in out_buf before construction.
         * TBFT_BLOCK_SIZE is user-configurable and may exceed
         * TBFT_MAX_MESSAGE_SIZE when combined with headers. */
        {
            size_t needed = sizeof(tbft_data_rep_t) + (size_t)TBFT_BLOCK_SIZE;
            if (needed > sizeof(r->out_buf)) {
                ESP_LOGE(TAG, "handle_fetch: Data response too large for out_buf"
                         " (%zu > %zu)", needed, sizeof(r->out_buf));
                return;
            }
        }

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

    /* Validate Meta_data message fits in out_buf before construction.
     * Even with TBFT_P_CHILDREN tuned to avoid overflow, the dynamic
     * count at sub-root levels can produce edge-case overruns. */
    {
        size_t needed = sizeof(tbft_meta_data_rep_t) + (size_t)count * sizeof(tbft_part_info_t);
        if (needed > sizeof(r->out_buf)) {
            ESP_LOGE(TAG, "handle_fetch: Meta_data response too large for out_buf"
                     " (%zu > %zu)", needed, sizeof(r->out_buf));
            return;
        }
    }

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

    /* If the root-level Meta_data showed all children's digests match,
     * no fetch requests were enqueued and the state is already in sync.
     * Check that ALL queue entries are done (not just len==0 — the root
     * entry stays in the queue with .done=true) and no Data is pending. */
    if (md->level == 0 && r->state.n_data_pending == 0) {
        bool all_done = true;
        for (int i = 0; i < r->state.fetch_queue_len; i++) {
            if (!r->state.fetch_queue[i].done) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            tbft_state_fetch_complete(&r->state);
            ESP_LOGI(TAG, "state fetch completed for seqno=%lld, marking stable",
                     (long long)r->state.fetch_seqno);
            tbft_replica_mark_stable(r, r->state.fetch_seqno);
            return;
        }
    }

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

    tbft_seqno_t fetch_seqno = r->state.fetch_seqno;

    tbft_state_handle_data(&r->state, data_rep, block_data);

    if (!tbft_state_in_fetch(&r->state)) {
        ESP_LOGI(TAG, "state fetch completed for seqno=%lld, marking stable", (long long)fetch_seqno);
        tbft_replica_mark_stable(r, fetch_seqno);
    }
}

/* --------------------------------------------------------------------------
 * Protocol actions
 * -------------------------------------------------------------------------- */

void tbft_replica_send_pre_prepare(tbft_replica_t *r)
{
    if (!tbft_replica_is_primary(r)) return;
    if (r->vi.in_progress) return; /* view-change in progress */

    /* Only require that we have at least threshold-1 peers with fresh keys.
     * The self-test already verifies each key on import, so no additional
     * settling delay is needed. */
    {
        int keys_ok = 0;
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            if (r->node.principals[i] && r->node.principals[i]->keys_fresh)
                keys_ok++;
        }
        if (keys_ok < r->node.threshold - 1) {
            return;
        }
    }

    /* Track which queue the request came from so we pop from the right one
     * after successfully building and broadcasting the Pre_prepare. */
    tbft_rqueue_t *src_queue = &r->rqueue;
    tbft_rqueue_entry_t *req = rqueue_front(src_queue);
    if (!req) {
        src_queue = &r->ro_rqueue;
        req = rqueue_front(src_queue);
    }
    if (!req) {
        /* No pending requests — stop the view-change timer so it doesn't
         * fire during idle periods.  The client sends every ~30s, so a 5s
         * timer would constantly trigger spurious view-changes. */
        tbft_itimer_stop(&r->vtimer);
        return;
    }

    /* Restart the vtimer now that we're actively working on a request. */
    tbft_itimer_start(&r->vtimer, r->vtimer_period_us);

    /* CRITICAL FIX: Dedup by request ID.  The same request can arrive
     * multiple times: once from the client's broadcast, then forwarded
     * by each non-primary replica.  Without dedup here, the primary
     * would assign a different seqno to each duplicate, rapidly draining
     * the sequence window and preventing consensus. */
    {
        const tbft_request_rep_t *rr = (const tbft_request_rep_t *)req->buf;
        if (rr->cid == r->last_assigned_cid && rr->rid == r->last_assigned_rid) {
            /* Already assigned a seqno for this request — drop duplicate. */
            rqueue_pop(src_queue);
            /* Check the other queue too */
            tbft_rqueue_entry_t *next_req = rqueue_front(src_queue);
            if (!next_req) {
                src_queue = (src_queue == &r->rqueue) ? &r->ro_rqueue : &r->rqueue;
                next_req = rqueue_front(src_queue);
            }
            if (!next_req) return;
            const tbft_request_rep_t *rr2 = (const tbft_request_rep_t *)next_req->buf;
            if (rr2->cid == r->last_assigned_cid && rr2->rid == r->last_assigned_rid) {
                rqueue_pop(src_queue);
                return;  /* all dups */
            }
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
        ESP_LOGE(TAG, "pre-prepare: request too large to embed, dropping (%zu > %zu)", aligned_needed, sizeof(r->out_buf));
        rqueue_pop(src_queue);
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

    /* Debug: log the key and auth offset used for each recipient */
    {
        int auth_off = (int)(ptr - r->out_buf);
        ESP_LOGI(TAG, "send_pp: tag=%d extra=%d size=%d ts=%lld, hdr[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x, auth_offset=%d",
                 pp->hdr.tag, pp->hdr.extra, pp->hdr.size,
                 (long long)pp->hdr.timestamp_us,
                 r->out_buf[0], r->out_buf[1], r->out_buf[2], r->out_buf[3],
                 r->out_buf[4], r->out_buf[5], r->out_buf[6], r->out_buf[7],
                 auth_off);
        for (int i = 0; i < r->node.num_replicas; i++) {
            if (i == r->node.node_id) continue;
            tbft_principal_t *p = r->node.principals[i];
            if (p) {
                ESP_LOGI(TAG, "send_pp: out_key for replica %d, key[0..3]=%02x%02x%02x%02x",
                         i, p->hmac_out_key.bytes[0], p->hmac_out_key.bytes[1],
                         p->hmac_out_key.bytes[2], p->hmac_out_key.bytes[3]);
            }
        }
    }

    /* Authenticator */
    tbft_auth_t *auth = (tbft_auth_t *)ptr;
    int32_t msg_len_before_auth = (int32_t)(ptr - r->out_buf);

    /* CRITICAL FIX: Set the message size BEFORE computing the HMAC.
     * The header's size field is part of the message bytes covered by
     * the MAC. If set after gen_auth, the sender signs size=0 but the
     * receiver verifies with the actual size → MAC mismatch. */
    pp->hdr.size = tbft_msg_align(msg_len_before_auth + (int32_t)sizeof(tbft_auth_t));

    tbft_node_gen_auth(&r->node, r->out_buf, (size_t)msg_len_before_auth,
                       auth);
    ptr += sizeof(tbft_auth_t);

    /* Store in agreement region */
    tbft_ar_store_pp(&r->ar, r->seqno, r->out_buf, pp->hdr.size);

    /* Broadcast */
    tbft_node_send(&r->node, r->out_buf, (size_t)pp->hdr.size,
                   TBFT_ALL_REPLICAS);

    ESP_LOGI(TAG, "sent pre-prepare seqno=%lld view=%lld (rset=%d, ndet=%d, total=%d)",
             (long long)r->seqno, (long long)r->node.view,
             req->len, ndet_len, pp->hdr.size);

    /* The primary's pre-prepare implicitly serves as its own Prepare.
     * Add a self-prepare to the certificate so the primary counts toward
     * the f+1 prepare threshold needed to reach the prepared state.
     * Also broadcast the prepare to all replicas so backups can reach
     * "prepared" state (they need f+1 prepares including the primary's). */
    {
        tbft_prepare_rep_t *prep = (tbft_prepare_rep_t *)r->out_buf;
        prep->hdr.tag   = TBFT_MSG_PREPARE;
        prep->hdr.extra = 0;
        prep->hdr.timestamp_us = esp_timer_get_time();
        prep->view      = r->node.view;
        prep->seqno     = r->seqno;
        prep->digest    = rset_digest;
        prep->id        = r->node.node_id;

        tbft_auth_t *auth2 = (tbft_auth_t *)(r->out_buf + sizeof(*prep));
        prep->hdr.size = tbft_msg_align(
            (int32_t)(sizeof(*prep) + sizeof(tbft_auth_t)));
        tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*prep), auth2);

        tbft_ar_add_my_prepare(&r->ar, r->seqno, r->out_buf, prep->hdr.size,
                               r->node.node_id);

        tbft_node_send(&r->node, r->out_buf, (size_t)prep->hdr.size,
                       TBFT_ALL_REPLICAS);
    }

    /* Mark this request ID as assigned to prevent duplicate seqno. */
    {
        const tbft_request_rep_t *rr_final = (const tbft_request_rep_t *)req->buf;
        r->last_assigned_cid = rr_final->cid;
        r->last_assigned_rid = rr_final->rid;
    }

    rqueue_pop(src_queue);

    /* The primary's last_prepared is advanced in tbft_replica_handle_prepare
     * once backup prepares are processed and the Prepared Certificate is complete.
     * This avoids prematurely advancing last_prepared before a quorum is reached. */

    r->seqno++;

    /* Reset view-change timer — primary is active */
    if (tbft_replica_has_pending_requests(r)) {
        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
    } else {
        tbft_itimer_stop(&r->vtimer);
    }
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
    prep->hdr.size = tbft_msg_align(
        (int32_t)(sizeof(*prep) + sizeof(tbft_auth_t)));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*prep), auth);

    /* Store own prepare; bail if already sent (duplicate PP delivery) */
    if (!tbft_ar_add_my_prepare(&r->ar, n, r->out_buf, prep->hdr.size,
                                r->node.node_id)) {
        return;
    }

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
    cm->hdr.size = tbft_msg_align(
        (int32_t)(sizeof(*cm) + sizeof(tbft_auth_t)));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*cm), auth);

    bool first_send = tbft_ar_add_my_commit(&r->ar, n, r->out_buf, cm->hdr.size,
                                            r->node.node_id);

    if (tbft_ar_in_range(&r->ar, n)) {
        tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, n);
        sl->commit_sent_us = esp_timer_get_time();
    }

    tbft_node_send(&r->node, r->out_buf, (size_t)cm->hdr.size,
                   TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "%s commit seqno=%lld",
             first_send ? "sent" : "re-sent", (long long)n);
}

void tbft_replica_execute_committed(tbft_replica_t *r)
{
    /* Execute in sequence-number order.
     * Loop bound: <= last_prepared (not +1) to avoid executing a slot that
     * has not yet been prepared. */
    for (tbft_seqno_t n = r->last_executed + 1;
         n <= r->last_prepared;
         n++) {
        if (!tbft_ar_committed(&r->ar, n)) {
            ESP_LOGW(TAG, "execute: seqno=%lld not committed yet"
                     " (last_executed=%lld, last_prepared=%lld) — break,"
                     " requesting fill",
                     (long long)n, (long long)r->last_executed,
                     (long long)r->last_prepared);
            if (r->pending_fill_seqno != n) {
                r->pending_fill_seqno  = n;
                r->fill_started_at_us  = esp_timer_get_time();
            }
            break;
        }

        int pp_len = 0;
        const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
        if (!pp_buf) {
            ESP_LOGW(TAG, "execute: no pre-prepare for seqno=%lld — break,"
                     " requesting fill",
                     (long long)n);
            if (r->pending_fill_seqno != n) {
                r->pending_fill_seqno  = n;
                r->fill_started_at_us  = esp_timer_get_time();
            }
            break;
        }

        const tbft_pre_prepare_rep_t *pp =
            (const tbft_pre_prepare_rep_t *)pp_buf;

        /* Defence-in-depth: verify the commit quorum's winning digest
         * matches the PP digest.  This catches Byzantine commits that
         * were accepted before the PP arrived and happen to reach quorum
         * with a wrong digest (impossible under pigeonhole with f+1
         * slots and only f faulty replicas, but checked anyway).
         * Poisoned seqnos are skipped; subsequent in-window seqnos
         * can still execute. */
        {
            tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, n);
            const uint8_t *winning_cm =
                tbft_commit_cert_cvalue(&sl->commit_cert);
            if (winning_cm) {
                const tbft_commit_rep_t *cm_win =
                    (const tbft_commit_rep_t *)winning_cm;
                if (!tbft_digest_equal(&pp->digest, &cm_win->digest)) {
                    ESP_LOGW(TAG, "execute: commit digest mismatch for seqno=%lld — skipping",
                             (long long)n);
        r->last_executed = n;

        /* Clear pending fill — the seqno we were waiting on is now executed */
        if (r->pending_fill_seqno == n) {
            r->pending_fill_seqno  = 0;
            r->fill_started_at_us  = 0;
        }
                    continue;
                }
            }
        }

        const uint8_t *req_bytes =
            (const uint8_t *)pp_buf + sizeof(*pp);
        int req_len   = pp->rset_size;
        int ndet_len  = pp->non_det_size;
        const uint8_t *ndet = req_bytes + req_len;

        /* Execute */
        if (r->exec_cb && req_len >= (int)sizeof(tbft_request_rep_t)) {
            ESP_LOGI(TAG, "execute: calling exec_cb for seqno=%lld req_len=%d",
                     (long long)n, req_len);
            const tbft_request_rep_t *req_rep =
                (const tbft_request_rep_t *)req_bytes;

            /* Use r->out_buf for reply; max reply payload fits inside.
             * Zero the buffer first so padding bytes between the reply
             * payload and the aligned hdr.size do not leak stack/heap
             * garbage into the RSA-signed data. */
            memset(r->out_buf, 0, sizeof(r->out_buf));
            tbft_reply_rep_t *reply = (tbft_reply_rep_t *)r->out_buf;
            uint8_t *rep_payload    = r->out_buf + sizeof(*reply);
            int      rep_len        = 0;
            bool     ro = (req_rep->hdr.extra & TBFT_REQUEST_RO_FLAG) != 0;

            int rc = r->exec_cb(req_bytes, req_len,
                                rep_payload, &rep_len,
                                (void *)ndet, ndet_len,
                                req_rep->cid, ro);

            if (rc != 0) {
                ESP_LOGW(TAG, "exec_cb rejected seqno=%lld", (long long)n);
                break;
            }

            /* Validate reply length AFTER exec_cb has set it */
            if (rep_len < 0 || sizeof(*reply) + (size_t)rep_len + TBFT_SIG_SIZE > sizeof(r->out_buf)) {
                ESP_LOGE(TAG, "reply too large for out_buf (%d + %d + %d > %zu)",
                         (int)sizeof(*reply), rep_len, (int)TBFT_SIG_SIZE, sizeof(r->out_buf));
                break;
            }
                reply->hdr.tag          = TBFT_MSG_REPLY;
                reply->hdr.extra        = 0;
                reply->hdr.timestamp_us = 0;
                reply->view             = r->node.view;
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
            ESP_LOGW(TAG, "execute: exec_cb=%p req_len=%d sizeof(req)=%d — skipping exec for seqno=%lld",
                     r->exec_cb, req_len, (int)sizeof(tbft_request_rep_t), (long long)n);
        }

        if (req_len >= (int)sizeof(tbft_request_rep_t)) {
            const tbft_request_rep_t *req_rep = (const tbft_request_rep_t *)req_bytes;
            if (req_rep->rid > r->last_executed_rid[req_rep->cid]) {
                r->last_executed_rid[req_rep->cid] = req_rep->rid;
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
            /* CRITICAL FIX: Set hdr.size BEFORE computing HMAC, same as
             * pre-prepare, prepare, and commit. The size field is part of
             * the message covered by the MAC. */
            ckpt->hdr.size = tbft_msg_align(
                (int32_t)(sizeof(*ckpt) + sizeof(tbft_auth_t)));
            tbft_node_gen_auth(&r->node, r->out_buf,
                               sizeof(*ckpt), auth);

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
    if (tbft_replica_has_pending_requests(r)) {
        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
    } else {
        tbft_itimer_stop(&r->vtimer);
    }
}

void tbft_replica_mark_stable(tbft_replica_t *r, tbft_seqno_t seqno)
{
    if (seqno <= r->last_stable) return;

    const tbft_digest_t *winning = tbft_cr_winning_digest(&r->cr, seqno);
    if (winning) {
        const tbft_digest_t *local = tbft_state_root_digest(&r->state);
        if (!tbft_digest_equal(winning, local)) {
            ESP_LOGW(TAG, "stable ckpt digest mismatch at seqno=%lld "
                     "winning=%02x%02x%02x%02x%02x%02x%02x%02x...%02x "
                     "local=%02x%02x%02x%02x%02x%02x%02x%02x...%02x "
                     "in_fetch=%d fetch_seqno=%lld",
                     (long long)seqno,
                     winning->bytes[0], winning->bytes[1],
                     winning->bytes[2], winning->bytes[3],
                     winning->bytes[4], winning->bytes[5],
                     winning->bytes[6], winning->bytes[7],
                     winning->bytes[TBFT_DIGEST_SIZE - 1],
                     local->bytes[0], local->bytes[1],
                     local->bytes[2], local->bytes[3],
                     local->bytes[4], local->bytes[5],
                     local->bytes[6], local->bytes[7],
                     local->bytes[TBFT_DIGEST_SIZE - 1],
                     (int)r->state.in_fetch,
                     (long long)r->state.fetch_seqno);
            if (!r->state.in_fetch || seqno > r->state.fetch_seqno) {
                /* Rate-limit fetch initiation to prevent a checkpoint
                 * mismatch detected by all replicas simultaneously from
                 * flooding the network.  The throttle is on the INITIATOR
                 * side — the replica that needs data paces its requests
                 * rather than silently dropping them at the responder.
                 * 500ms between fetch starts gives the network time to
                 * deliver the response before the next request. */
                int64_t now_us = esp_timer_get_time();
                if (now_us - r->last_fetch_throttle_us < 500000LL) return;
                r->last_fetch_throttle_us = now_us;

                tbft_replica_start_fetch(r, seqno,
                                        tbft_node_primary(&r->node, r->node.view));
            }
            return;
        }
    }

    tbft_state_mark_stable(&r->state, seqno);
    r->last_stable = seqno;

    /* Advance execution and prepared markers to the stable checkpoint.
     * After a state fetch, the application state matches this checkpoint
     * but last_executed and last_prepared can lag behind (stuck at 0 on a
     * freshly-booted node that jumped from seqno 0 to 1000).  Without this,
     * the execution loop starts far below the agreement window head and
     * either finds uncommitted gaps or falls permanently out of range. */
    if (r->last_prepared < seqno) r->last_prepared = seqno;
    if (r->last_executed < seqno) r->last_executed = seqno;

    /* Clear any pending fill request — its seqno is now at or below
     * last_executed / last_stable, so the main loop will not re-send. */
    if (r->pending_fill_seqno > 0 && r->pending_fill_seqno <= seqno) {
        r->pending_fill_seqno  = 0;
        r->fill_started_at_us  = 0;
    }

    /* Truncate static regions */
    tbft_ar_truncate(&r->ar, seqno + 1);
    tbft_cr_truncate(&r->cr, seqno);

    /* Reset view-change timer — stable progress */
    if (tbft_replica_has_pending_requests(r)) {
        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
    } else {
        tbft_itimer_stop(&r->vtimer);
    }

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
    r->vi.in_progress = true;

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

    /* Exponential backoff: each consecutive view change doubles the
     * vtimer period relative to the base, capped at MAX_MULT.
     * The shift is bounded by the view-gap so the timer grows with
     * each successive failure rather than resetting to 2x every time. */
    int shift = (int)(target - r->node.view);
    if (shift < 1) shift = 1;
    if (shift > 6) shift = 6;
    int64_t next_period = r->vtimer_period_us * (1LL << shift);
    if (next_period > r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT) {
        next_period = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
    }
    tbft_itimer_start(&r->vtimer, next_period);
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

        /* Generate a cryptographically random session key.
         * CRITICAL FIX: Only generate a new key if the out-key is not yet set.
         * Re-broadcasts of New_key must use the SAME key — otherwise the
         * primary's hmac_out_key changes every 2s while backups are still
         * decrypting and installing keys from older broadcasts, causing
         * a key mismatch and "pp: MAC verification failed". */
        tbft_hmac_key_t new_key;
        bool need_new_key = true;
        for (int b = 0; b < (int)sizeof(p->hmac_out_key.bytes); b++) {
            if (p->hmac_out_key.bytes[b] != 0) {
                need_new_key = false;
                break;
            }
        }
        if (need_new_key) {
            esp_fill_random(new_key.bytes, sizeof(new_key.bytes));
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
            /* Cache the generated ciphertext so future re-broadcasts send the exact same bytes */
            memcpy(p->last_sent_new_key_ciphertext, slots[slot_idx].ciphertext, TBFT_SIG_SIZE);
        } else {
            new_key = p->hmac_out_key;
            /* Copy the previously generated ciphertext to keep it byte-for-byte identical */
            memcpy(slots[slot_idx].ciphertext, p->last_sent_new_key_ciphertext, TBFT_SIG_SIZE);
        }

        /* Store locally as the out-key for messages we send TO replica i.
         * Must happen AFTER successful encryption so peers that fail
         * encryption don't get a stale out-key set locally. */
        tbft_principal_set_out_key(p, &new_key);
        ESP_LOGI(TAG, "send_new_key: set out_key for replica %d (new=%d)", i, need_new_key);

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

    tbft_principal_t *p = r->node.principals[sender_id];
    if (!p) return;

    int expected_len = (int)(sizeof(*nk)
                             + (size_t)nk->n_keys * sizeof(tbft_new_key_slot_t));
    if (len < expected_len) {
        ESP_LOGW(TAG, "new_key from %d: message too short", sender_id);
        return;
    }

    const tbft_new_key_slot_t *slots =
        (const tbft_new_key_slot_t *)((const uint8_t *)msg + sizeof(*nk));

    /* Find the slot addressed to us */
    int our_slot_idx = -1;
    for (int i = 0; i < nk->n_keys; i++) {
        if (slots[i].recipient_id == r->node.node_id) {
            our_slot_idx = i;
            break;
        }
    }

    if (our_slot_idx == -1) {
        return; /* not addressed to us — ignore */
    }

    /* Fast path: check if this is a duplicate of the last successfully processed New_key.
     * Since RSA-OAEP uses random padding, different sessions produce completely different
     * ciphertexts. If the ciphertext is exactly identical, and our key is already fresh,
     * we are guaranteed that the key hasn't changed. We can safely skip the extremely
     * slow RSA signature verification (~800ms) and RSA decryption (~800ms). */
    if (p->keys_fresh && memcmp(p->last_new_key_ciphertext, slots[our_slot_idx].ciphertext, TBFT_SIG_SIZE) == 0) {
        return;
    }

    /* Slow path: verify RSA signature first to prevent authentication bypass. */
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

    /* Decrypt with our RSA private key */
    tbft_principal_t *local = r->node.local_principal;
    if (!local) return;

    tbft_hmac_key_t new_key;
    int rc = tbft_principal_decrypt_new_key(local,
                                             slots[our_slot_idx].ciphertext,
                                             TBFT_SIG_SIZE,
                                             &new_key);
    if (rc != 0) {
        ESP_LOGW(TAG, "new_key: decrypt from replica %d failed", sender_id);
        return;
    }

    /* Install as the in-key for verifying messages FROM sender_id */
    ESP_LOGI(TAG, "handle_new_key: from replica %d, decrypted key[0..3]=%02x%02x%02x%02x",
             sender_id, new_key.bytes[0], new_key.bytes[1],
             new_key.bytes[2], new_key.bytes[3]);
    tbft_principal_set_in_key(p, &new_key);
    
    /* Save the ciphertext to skip duplicate processing in the future */
    memcpy(p->last_new_key_ciphertext, slots[our_slot_idx].ciphertext, TBFT_SIG_SIZE);

    ESP_LOGI(TAG, "installed HMAC in-key from replica %d", sender_id);

    /* Explicit wipe of plaintext key after installation */
    memset(&new_key, 0, sizeof(new_key));
}

/* --------------------------------------------------------------------------
 * Fill_request handler
 *
 * A backup asks the primary to re-send the pre-prepare it previously
 * broadcast for `seqno`.  This is the recovery path for the gap-stall bug:
 * when commits arrive out of order (e.g., seqno N+1 commits before N), the
 * execution loop breaks on N and never advances.  By re-sending the stored
 * pre-prepare, the primary lets the backup run the normal prepare/commit
 * flow and close the gap without a view-change.
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_fill_request(tbft_replica_t *r,
                                      const void *msg, int len)
{
    if (len < (int)sizeof(tbft_fill_request_rep_t)) {
        ESP_LOGW(TAG, "fill: message too short (%d < %d)",
                 len, (int)sizeof(tbft_fill_request_rep_t));
        return;
    }
    const tbft_fill_request_rep_t *fr =
        (const tbft_fill_request_rep_t *)msg;

    /* Validate requester id */
    if (fr->id < 0 || fr->id >= r->node.num_replicas
        || fr->id == r->node.node_id) {
        ESP_LOGW(TAG, "fill: invalid requester id %d", fr->id);
        return;
    }

    /* Verify HMAC of the request using the requester's principal.
     * Uses the requester's session in-key (which equals this replica's
     * out-key to the requester, by New_key handshake symmetry). */
    tbft_principal_t *p = r->node.principals[fr->id];
    if (!p || !p->keys_fresh) {
        ESP_LOGD(TAG, "fill: dropping request from %d (key not fresh)",
                 fr->id);
        return;
    }
    int32_t body_len = (int32_t)(sizeof(*fr) - sizeof(tbft_mac_t));
    if (!tbft_principal_verify_mac_in(p, msg, (size_t)body_len, &fr->mac)) {
        ESP_LOGW(TAG, "fill: HMAC verify failed from replica %d", fr->id);
        return;
    }

    /* v0.2.14 (Layer 2 broadcast-fill): any replica that received the
     * original PP can re-send it. This lets backups help recover from
     * the "current primary never had the PP" case (e.g., the original
     * sender crashed mid-broadcast, and subsequent view changes do not
     * bring a primary that has the PP).
     *
     * The PP's HMAC was computed by the original primary; we forward the
     * original bytes unchanged. The requester verifies via the standard
     * handle_pre_prepare path. The PP's view field is whatever view the
     * original primary assigned — if the requester is in a different
     * view, it will reject (this is correct PBFT behaviour). */
    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, fr->seqno, &pp_len);
    if (!pp_buf) {
        int primary = tbft_node_primary(&r->node, fr->view);
        if (primary != r->node.node_id) {
            ESP_LOGD(TAG,
                "fill: not primary and no stored PP for seqno=%lld"
                " (view=%lld primary=%d me=%d)",
                (long long)fr->seqno, (long long)fr->view,
                primary, r->node.node_id);
        } else {
            ESP_LOGW(TAG, "fill: no stored pre-prepare for seqno=%lld"
                     " (out of window or not yet assigned)",
                     (long long)fr->seqno);
        }
        return;
    }

    /* Re-send the original PP bytes (includes authenticator).
     * The receiver will verify via the standard handle_pre_prepare path. */
    tbft_node_send(&r->node, pp_buf, (size_t)pp_len, fr->id);
    ESP_LOGI(TAG,
        "fill: re-sent pre-prepare seqno=%lld to replica %d (size=%d, view=%lld)",
        (long long)fr->seqno, fr->id, pp_len, (long long)fr->view);
}
