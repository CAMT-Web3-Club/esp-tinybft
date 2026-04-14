#include "tbft_replica.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "tbft_replica";

/* --------------------------------------------------------------------------
 * Request queue helpers
 * -------------------------------------------------------------------------- */

static void rqueue_init(tbft_rqueue_t *q) {
    memset(q, 0, sizeof(*q));
}

static bool rqueue_push(tbft_rqueue_t *q, const void *buf, int len, bool ro) {
    if (q->count >= TBFT_RQUEUE_MAX) return false;
    tbft_rqueue_entry_t *e = &q->entries[q->tail];
    if (len > TBFT_MAX_MESSAGE_SIZE) return false;
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

    /* Timer periods (defaults — overridden by Byz_init_replica from config) */
    r->vtimer_period_us = 5000000LL;  /* 5 s */
    r->stimer_period_us = 1000000LL;  /* 1 s */

    /* Init timers (not started — caller must set periods and start) */
    tbft_itimer_init(&r->vtimer, vtimer_cb, r, "tbft_vtimer");
    tbft_itimer_init(&r->stimer, stimer_cb, r, "tbft_stimer");
    tbft_itimer_init(&r->rtimer, NULL,       r, "tbft_rtimer");
    tbft_itimer_init(&r->ntimer, NULL,       r, "tbft_ntimer");

    r->evt_group = xEventGroupCreate();
    if (!r->evt_group) {
        ESP_LOGE(TAG, "replica: failed to create event group");
        tbft_itimer_free(&r->vtimer);
        tbft_itimer_free(&r->stimer);
        tbft_itimer_free(&r->rtimer);
        tbft_itimer_free(&r->ntimer);
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
    tbft_itimer_free(&r->rtimer);
    tbft_itimer_free(&r->ntimer);
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

    /* Broadcast fresh HMAC session keys to all peers before processing
     * any protocol messages.  Peers will also send their own New_key
     * messages which arrive during the normal event loop below. */
    tbft_replica_send_new_key(r);

    /* Subscribe this task to the task watchdog so we can periodically feed it.
     * The default timeout is CONFIG_ESP_TASK_WDT_TIMEOUT_S (typically 5s). */
#if CONFIG_ESP_TASK_WDT_EN
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to subscribe to task WDT: %d", err);
    }
#endif

    while (r->running) {
#if CONFIG_ESP_TASK_WDT_EN
        esp_task_wdt_reset();
#endif

        /* Periodic yield counter: after processing a burst of messages,
         * yield to let lower-priority tasks (send_task at priority 3)
         * get CPU time.  Without this, the replica (priority 5) can
         * starve the send_task indefinitely under continuous message load. */
        static int msg_count = 0;
        if (++msg_count >= 16) {
            msg_count = 0;
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (r->evt_group) {
            EventBits_t bits = xEventGroupClearBits(r->evt_group, TBFT_EVT_VTIMER | TBFT_EVT_STIMER);
            if (bits & TBFT_EVT_VTIMER) {
                ESP_LOGW(TAG, "view-change timeout in view %lld", (long long)r->node.view);
                tbft_replica_send_view_change(r);
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

        tbft_node_id_t src_id = -1;
        int n = tbft_node_recv(&r->node, r->node.recv_buf, &src_id);
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
}

/* --------------------------------------------------------------------------
 * Request handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_request(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_request_rep_t)) return;
    const tbft_request_rep_t *req = (const tbft_request_rep_t *)msg;

    if (req->command_size < 0 ||
        (size_t)req->command_size > TBFT_MAX_MESSAGE_SIZE - sizeof(*req) - sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "request: invalid command_size %d", req->command_size);
        return;
    }

    if (req->cid < 0 || req->cid >= r->node.num_principals) {
        ESP_LOGW(TAG, "request: invalid client id %d", req->cid);
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

    /* If we are not the primary, forward to the primary */
    if (!tbft_replica_is_primary(r)) {
        int primary = tbft_node_primary(&r->node, r->node.view);
        tbft_node_send(&r->node, msg, (size_t)len, primary);
        /* Also restart view-change timer in case primary is faulty */
        tbft_itimer_start(&r->vtimer, r->vtimer_period_us);
        return;
    }

    /* Primary: enqueue and start ordering */
    bool ro = (req->hdr.extra & 0x1) != 0;
    tbft_rqueue_t *q = ro ? &r->ro_rqueue : &r->rqueue;
    if (!rqueue_push(q, msg, len, ro)) {
        ESP_LOGW(TAG, "request queue full, dropping request from client %d",
                 req->cid);
        return;
    }

    /* Try to send a pre-prepare if window allows */
    tbft_replica_send_pre_prepare(r);
}

/* --------------------------------------------------------------------------
 * Pre-prepare handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_pre_prepare(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_pre_prepare_rep_t)) return;
    const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)msg;

    /* Reject if wrong view */
    if (pp->view != r->node.view) return;

    /* Must come from current primary */
    int expected_primary = tbft_node_primary(&r->node, pp->view);
    if (r->node.node_id == expected_primary) return; /* primary ignores own PP */

    /* Check sequence number is in window */
    if (!tbft_replica_in_window(r, pp->seqno)) {
        ESP_LOGD(TAG, "pp seqno %lld out of window", (long long)pp->seqno);
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
        ESP_LOGW(TAG, "pp: embedded sizes exceed message length");
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
        if (slot < 0) return;
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

    /* Verify request set digest */
    if (pp->rset_size > 0) {
        const uint8_t *req_set = (const uint8_t *)msg + sizeof(*pp);
        tbft_digest_t computed;
        tbft_msg_digest(req_set, (size_t)pp->rset_size, &computed);
        if (!tbft_digest_equal(&pp->digest, &computed)) {
            ESP_LOGW(TAG, "pp: request set digest mismatch");
            return;
        }
    }

    /* Store in agreement region */
    if (!tbft_ar_store_pp(&r->ar, pp->seqno, msg, len)) {
        return;
    }

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
    if (len < (int)sizeof(tbft_prepare_rep_t) + (int)sizeof(tbft_auth_t)) return;
    const tbft_prepare_rep_t *prep = (const tbft_prepare_rep_t *)msg;

    if (prep->view != r->node.view) return;
    if (!tbft_replica_in_window(r, prep->seqno)) return;

    tbft_node_id_t sender = prep->id;
    if (sender < 0 || sender >= r->node.num_replicas) {
        ESP_LOGW(TAG, "prepare: invalid sender id %d", sender);
        return;
    }
    if (sender == tbft_node_primary(&r->node, prep->view)) return;
    if (sender == r->node.node_id) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) return;

    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*prep));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*prep),
                               &auth->slots[slot], hdr->timestamp_us)) {
        ESP_LOGW(TAG, "prepare: MAC verification failed from %d", sender);
        return;
    }

    bool accepted = tbft_ar_add_prepare(&r->ar, prep->seqno, msg, len, sender);
    if (!accepted) return;

    /* Check if prepared — if so, send commit */
    if (tbft_ar_prepared(&r->ar, prep->seqno)) {
        ESP_LOGD(TAG, "prepared seqno=%lld", (long long)prep->seqno);
        tbft_replica_send_commit(r, prep->seqno);
    }
}

/* --------------------------------------------------------------------------
 * Commit handler
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_commit(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_commit_rep_t) + (int)sizeof(tbft_auth_t)) return;
    const tbft_commit_rep_t *cm = (const tbft_commit_rep_t *)msg;

    if (cm->view != r->node.view) return;
    if (!tbft_replica_in_window(r, cm->seqno)) return;

    tbft_node_id_t sender = cm->id;
    if (sender < 0 || sender >= r->node.num_replicas) {
        ESP_LOGW(TAG, "commit: invalid sender id %d", sender);
        return;
    }
    if (sender == r->node.node_id) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) return;

    const tbft_msg_hdr_t *hdr = (const tbft_msg_hdr_t *)msg;
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*cm));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*cm),
                               &auth->slots[slot], hdr->timestamp_us)) {
        ESP_LOGW(TAG, "commit: MAC verification failed from %d", sender);
        return;
    }

    bool accepted = tbft_ar_add_commit(&r->ar, cm->seqno, msg, len, sender);
    if (!accepted) return;

    /* Check if committed-local */
    if (tbft_ar_committed(&r->ar, cm->seqno)) {
        ESP_LOGD(TAG, "committed seqno=%lld", (long long)cm->seqno);
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

    if (!tbft_vi_collect_vc(&r->vi, sender_id, msg, len)) return;

    /* If we are the new primary and have enough view-changes, send new-view */
    int new_primary = tbft_node_primary(&r->node, vc->v);
    if (new_primary == r->node.node_id && tbft_vi_has_quorum(&r->vi)) {
        /* Build and send New_view */
        tbft_seqno_t min_s, max_s;
        tbft_vi_compute_min_max(&r->vi, &min_s, &max_s);

        tbft_new_view_rep_t *nv = (tbft_new_view_rep_t *)r->out_buf;
        nv->hdr.tag   = TBFT_MSG_NEW_VIEW;
        nv->hdr.extra = 0;
        nv->v         = vc->v;
        nv->min       = min_s;
        nv->max       = max_s;
        nv->n_prep    = 0;
        nv->has_sig   = 1;

        /* Sign the New_view */
        tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + sizeof(*nv));
        if (tbft_node_gen_sig(&r->node, nv, sizeof(*nv), sig) == 0) {
            nv->hdr.size = tbft_msg_align(
                (int32_t)(sizeof(*nv) + sizeof(tbft_sig_t)));
        } else {
            nv->has_sig = 0;
            nv->hdr.size = tbft_msg_align((int32_t)sizeof(*nv));
        }

        tbft_node_send(&r->node, r->out_buf, (size_t)nv->hdr.size,
                       TBFT_ALL_REPLICAS);
        ESP_LOGI(TAG, "sent new-view v=%lld min=%lld max=%lld",
                 (long long)vc->v, (long long)min_s, (long long)max_s);
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

    /* Verify signature from the new primary */
    if (nv->has_sig) {
        int sig_offset = (int)((const uint8_t *)nv - (const uint8_t *)msg)
                       + sizeof(*nv);
        if (len < sig_offset + (int)sizeof(tbft_sig_t)) {
            ESP_LOGW(TAG, "new-view: missing signature");
            return;
        }
        const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + sig_offset);
        int new_primary = tbft_node_primary(&r->node, nv->v);
        if (!tbft_node_verify_sig(&r->node, new_primary,
                                  nv, sizeof(*nv), sig)) {
            ESP_LOGW(TAG, "new-view signature verification failed for v=%lld",
                     (long long)nv->v);
            return;
        }
    }

    /* Install new view */
    r->node.view        = nv->v;
    r->node.cur_primary = tbft_node_primary(&r->node, nv->v);

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

    tbft_rqueue_entry_t *req = rqueue_front(&r->rqueue);
    if (!req) req = rqueue_front(&r->ro_rqueue);
    if (!req) return;

    /* Check window */
    if (!tbft_replica_in_window(r, r->seqno)) {
        ESP_LOGD(TAG, "seqno %lld out of window, waiting", (long long)r->seqno);
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

    /* Guard against buffer overflow before building the message */
    size_t needed = sizeof(tbft_pre_prepare_rep_t) + (size_t)req->len
                  + (size_t)ndet_len + sizeof(tbft_auth_t);
    size_t aligned_needed = (needed + 7u) & ~7u;
    if (aligned_needed > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "pre-prepare: message too large (%zu > %zu)", aligned_needed, sizeof(r->out_buf));
        return;
    }

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

    ESP_LOGD(TAG, "sent pre-prepare seqno=%lld view=%lld",
             (long long)r->seqno, (long long)r->node.view);

    rqueue_pop(&r->rqueue);
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
    ESP_LOGD(TAG, "sent prepare seqno=%lld", (long long)n);
}

void tbft_replica_send_commit(tbft_replica_t *r, tbft_seqno_t n)
{
    tbft_commit_rep_t *cm = (tbft_commit_rep_t *)r->out_buf;
    cm->hdr.tag   = TBFT_MSG_COMMIT;
    cm->hdr.extra = 0;
    cm->hdr.timestamp_us = esp_timer_get_time();
    cm->view      = r->node.view;
    cm->seqno     = n;
    cm->id        = r->node.node_id;

    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*cm));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*cm), auth);
    cm->hdr.size = tbft_msg_align(
        (int32_t)(sizeof(*cm) + sizeof(tbft_auth_t)));

    tbft_ar_add_my_commit(&r->ar, n, r->out_buf, cm->hdr.size,
                          r->node.node_id);

    tbft_node_send(&r->node, r->out_buf, (size_t)cm->hdr.size,
                   TBFT_ALL_REPLICAS);
    ESP_LOGD(TAG, "sent commit seqno=%lld", (long long)n);
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
            bool     ro = (req_rep->hdr.extra & 0x1) != 0;

            /* Ensure enough room: hdr + payload + sig */
            if (sizeof(*reply) + TBFT_MAX_REPLY_SIZE + TBFT_SIG_SIZE
                    > sizeof(r->out_buf)) {
                ESP_LOGE(TAG, "out_buf too small for reply");
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
                    }
                }

                if (r->recv_reply_cb) {
                    r->recv_reply_cb(r->out_buf, total, req_rep->cid);
                }
                tbft_node_send(&r->node, r->out_buf, (size_t)total,
                               req_rep->cid);
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
    tbft_view_t new_view = r->node.view + 1;
    tbft_vi_reset(&r->vi, new_view);

    uint8_t *ptr = r->out_buf;
    tbft_view_change_rep_t *vc = (tbft_view_change_rep_t *)ptr;
    vc->hdr.tag   = TBFT_MSG_VIEW_CHANGE;
    vc->hdr.extra = 0;
    vc->v         = new_view;
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

    int max_reqs = (int)((sizeof(r->out_buf) - (size_t)(ptr - r->out_buf) - sizeof(tbft_sig_t)) / sizeof(tbft_vc_req_info_t));
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
     * NOT included in hdr.size so receivers can locate it as msg + hdr.size. */
    int32_t body_size  = vc->hdr.size;
    int32_t total_size = body_size + (int32_t)sizeof(tbft_sig_t);
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + body_size);
    if (total_size <= (int32_t)sizeof(r->out_buf)) {
        if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)body_size, sig) != 0) {
            ESP_LOGW(TAG, "send_view_change: failed to sign message");
            total_size = body_size; /* send unsigned if signing fails */
        }
    } else {
        ESP_LOGW(TAG, "send_view_change: out_buf too small for signature");
        total_size = body_size;
    }

    tbft_node_send(&r->node, r->out_buf, (size_t)total_size, TBFT_ALL_REPLICAS);

    ESP_LOGI(TAG, "sent view-change to view %lld", (long long)new_view);

    /* Collect our own view-change (pass full wire length including sig) */
    tbft_vi_collect_vc(&r->vi, r->node.node_id, r->out_buf, total_size);

    /* Start view-change retransmit timer */
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
            /* Zero the ciphertext to avoid leaking key material */
            memset(slots[slot_idx].ciphertext, 0,
                   sizeof(slots[slot_idx].ciphertext));
        }

        slots[slot_idx].recipient_id = i;
        slot_idx++;
    }

    if (slot_idx == 0) return;

    nk->n_keys = slot_idx;
    int32_t total = (int32_t)(sizeof(*nk)
                               + (size_t)slot_idx * sizeof(tbft_new_key_slot_t));
    nk->hdr.size = tbft_msg_align(total);

    tbft_node_send(&r->node, r->out_buf, (size_t)nk->hdr.size,
                   TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent New_key with %d slots", slot_idx);
}

void tbft_replica_handle_new_key(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_new_key_rep_t)) return;
    const tbft_new_key_rep_t *nk = (const tbft_new_key_rep_t *)msg;

    tbft_node_id_t sender_id = (tbft_node_id_t)nk->id;
    if (sender_id < 0 || sender_id >= r->node.num_replicas) return;
    if (sender_id == r->node.node_id) return; /* ignore own broadcasts */

    if (nk->n_keys <= 0 || nk->n_keys > r->node.num_replicas) return;

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
            ESP_LOGI(TAG, "installed HMAC in-key from replica %d", sender_id);
        }

        /* Explicit wipe of plaintext key after installation */
        memset(&new_key, 0, sizeof(new_key));
        return; /* only one slot per sender per message */
    }
}
