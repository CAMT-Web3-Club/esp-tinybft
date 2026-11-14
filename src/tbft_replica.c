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
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static constexpr char TAG[] = "tbft_replica";

/* Low #8 FIX: Named constants for magic numbers */
static constexpr int TBFT_YIELD_THRESHOLD     = 16;  /* message loop iterations before cooperative yield */
static constexpr int TBFT_VC_BACKOFF_MAX_MULT = 60;  /* max multiplier for view-change timeout backoff */

/* M2 FIX: Read-only request flag in hdr.extra field */
static constexpr int TBFT_REQUEST_RO_FLAG = 0x1;

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
    if (q->count == 0) return nullptr;
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
    /* A new view starts with no pending request: requests that were not
     * ordered come back with the client's retransmission (and then start the
     * view-change timer again).  A request whose client gave up or went away
     * must not keep the timer running in every later view. */
    for (int i = 0; i < r->node.num_principals; i++) {
        r->last_forwarded_rid[i] = r->last_executed_rid[i];
        r->last_received_rid[i]  = r->last_executed_rid[i];
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
    if (tbft_state_init(&r->state, state_mem, state_size,
                        r->sys_state, TBFT_SYS_STATE_SIZE) != 0) {
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
    r->last_fetch_throttle_us    = 0;
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
    if (tbft_itimer_init(&r->vtimer, vtimer_cb, r, "tbft_vtimer") != ESP_OK ||
        tbft_itimer_init(&r->stimer, stimer_cb, r, "tbft_stimer") != ESP_OK) {
        ESP_LOGE(TAG, "replica: timer init failed");
        tbft_itimer_free(&r->vtimer);
        tbft_itimer_free(&r->stimer);
        tbft_state_free(&r->state);
        tbft_node_free(&r->node);
        return -1;
    }

    r->evt_group = xEventGroupCreateStatic(&r->evt_group_buf);
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
        r->evt_group = nullptr;
    }
    tbft_state_free(&r->state);
    tbft_node_free(&r->node);
}

/* --------------------------------------------------------------------------
 * Message dispatch
 * -------------------------------------------------------------------------- */

static void handle_status(tbft_replica_t *r, const void *msg, int len);
static void send_view_change_to(tbft_replica_t *r, tbft_view_t target);
static void maybe_send_new_view(tbft_replica_t *r);

static void tbft_replica_start_fetch(tbft_replica_t *r, tbft_seqno_t seqno, int replier,
                                     const tbft_digest_t *digest)
{
    r->stat_fetch_started++;
    tbft_digest_t target;
    if (digest) target = *digest;   /* may point into r->state */
    tbft_state_start_fetch(&r->state, seqno, replier, digest ? &target : nullptr);

    /* Send initial fetch request(s) */
    int level, index;
    while (tbft_state_next_fetch_req(&r->state, &level, &index)) {
        tbft_fetch_rep_t *fr = (tbft_fetch_rep_t *)r->out_buf;
        fr->hdr.tag     = TBFT_MSG_FETCH;
        fr->hdr.extra   = 0;
        fr->last_stable = r->last_stable;
        fr->c           = r->state.fetch_seqno;
        fr->level       = level;
        fr->index       = index;
        fr->id          = r->node.node_id;
        fr->hdr.size    = tbft_msg_align((int32_t)sizeof(*fr));
        tbft_node_send(&r->node, r->out_buf, (size_t)fr->hdr.size,
                       r->state.fetch_replier);
    }
}

/* --------------------------------------------------------------------------
 * Helpers shared by the handlers below
 * -------------------------------------------------------------------------- */

static int count_fresh_keys(const tbft_replica_t *r)
{
    int keys_ok = 0;
    for (int i = 0; i < r->node.num_replicas; i++) {
        if (i == r->node.node_id) continue;
        if (r->node.principals[i] && r->node.principals[i]->keys_fresh) keys_ok++;
    }
    return keys_ok;
}

static bool digest_is_zero(const tbft_digest_t *d)
{
    tbft_digest_t zero;
    tbft_digest_zero(&zero);
    return tbft_digest_equal(d, &zero);
}

/* Client table index of client @p cid, or -1 when @p cid is not a client.
 * Clients follow the replicas in the node list. */
static int client_index(const tbft_replica_t *r, int cid)
{
    if (cid < r->node.num_replicas || cid >= r->node.num_principals) return -1;
    int idx = cid - r->node.num_replicas;
    return idx < TBFT_MAX_NUM_CLIENTS ? idx : -1;
}

static tbft_client_entry_t *client_entry(tbft_replica_t *r, int idx)
{
    return (tbft_client_entry_t *)r->sys_state + idx;
}

/* Mark the state blocks that hold client entry @p idx as modified. */
static void client_entry_touch(tbft_replica_t *r, int idx)
{
    size_t off   = (size_t)idx * sizeof(tbft_client_entry_t);
    int    first = r->state.app_blocks + (int)(off / TBFT_BLOCK_SIZE);
    int    last  = r->state.app_blocks +
                   (int)((off + sizeof(tbft_client_entry_t) - 1) / TBFT_BLOCK_SIZE);
    for (int b = first; b <= last; b++) {
        tbft_state_cow_single(&r->state, b);
    }
}

/* Bring the per-client counters kept outside the replicated state in line
 * with the client table (after a state fetch replaced it). */
static void sync_executed_rids(tbft_replica_t *r)
{
    for (int cid = r->node.num_replicas; cid < r->node.num_principals; cid++) {
        int idx = client_index(r, cid);
        if (idx < 0) continue;
        tbft_req_id_t rid = client_entry(r, idx)->last_rid;
        if (rid > r->last_executed_rid[cid]) r->last_executed_rid[cid] = rid;
        if (rid > r->last_assigned_rid[cid]) r->last_assigned_rid[cid] = rid;
    }
}

static constexpr int64_t TBFT_REPLY_RESEND_MIN_US = 1'000'000;
static constexpr int64_t TBFT_ASK_INTERVAL_US     = 1'000'000;

/* Send client @p cid the reply recorded in its entry @p e.  The signed
 * message is cached in the special region, so a retransmitted request is
 * answered again without another RSA signature. */
static void send_reply(tbft_replica_t *r, int cid, const tbft_client_entry_t *e, bool first)
{
    int idx = client_index(r, cid);
    if (idx < 0 || e->reply_len < 0 || e->reply_len > TBFT_MAX_REPLY_SIZE) return;

    int64_t now = esp_timer_get_time();
    if (!first) {
        /* Copies of the request that arrive right after it ran (forwarded
         * by backups) are not retransmissions: the client has its reply. */
        if (now - r->reply_sent_us[idx] < TBFT_REPLY_RESEND_MIN_US) return;
        r->reply_sent_us[idx] = now;
        int cached_len = 0;
        const uint8_t *cached = tbft_sr_load_reply(&r->sr, idx, &cached_len);
        if (cached && cached_len >= (int)sizeof(tbft_reply_rep_t) &&
            ((const tbft_reply_rep_t *)cached)->rid == e->last_rid) {
            ESP_LOGI(TAG, "re-sending reply: cid=%d rid=%llu", cid,
                     (unsigned long long)e->last_rid);
            tbft_node_send(&r->node, cached, (size_t)cached_len, cid);
            return;
        }
    }

    memset(r->out_buf, 0, sizeof(r->out_buf));
    tbft_reply_rep_t *reply = (tbft_reply_rep_t *)r->out_buf;
    reply->hdr.tag          = TBFT_MSG_REPLY;
    reply->hdr.extra        = 0;
    reply->hdr.timestamp_us = 0;
    reply->view             = r->node.view;
    reply->seqno            = e->seqno;
    reply->cid              = cid;
    reply->rid              = e->last_rid;
    reply->reply_size       = e->reply_len;
    memcpy(r->out_buf + sizeof(*reply), e->reply, (size_t)e->reply_len);
    reply->hdr.size = tbft_msg_align((int32_t)(sizeof(*reply) + (size_t)e->reply_len));
    if ((size_t)reply->hdr.size + sizeof(tbft_sig_t) > sizeof(r->out_buf)) return;

    /* Sign the reply so the client can verify authenticity */
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + reply->hdr.size);
    if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)reply->hdr.size, sig) != 0) {
        ESP_LOGE(TAG, "reply signing failed for cid=%d rid=%llu - dropping reply", cid,
                 (unsigned long long)e->last_rid);
        return;
    }
    int32_t total = reply->hdr.size + (int32_t)sizeof(tbft_sig_t);
    r->reply_sent_us[idx] = now;
    (void)tbft_sr_store_reply(&r->sr, idx, r->out_buf, total);
    ESP_LOGI(TAG, "%s reply: cid=%d rid=%llu seqno=%lld size=%d",
             first ? "sending" : "re-sending", cid, (unsigned long long)e->last_rid,
             (long long)e->seqno, (int)total);
    if (first && r->recv_reply_cb) {
        r->recv_reply_cb(r->out_buf, total, cid);
    }
    tbft_node_send(&r->node, r->out_buf, (size_t)total, cid);
}

/* View-change timer period: the configured one, doubled for every view
 * change in a row that brought no progress (capped). */
static int64_t vtimer_period(const tbft_replica_t *r)
{
    int shift = r->vc_backoff_shift > 6 ? 6 : r->vc_backoff_shift;
    int64_t period = r->vtimer_period_us << shift;
    int64_t cap    = r->vtimer_period_us * TBFT_VC_BACKOFF_MAX_MULT;
    return period < cap ? period : cap;
}

/* Run the view-change timer only while a request waits for execution
 * (restarting it: called when a request has just executed). */
static void update_vtimer(tbft_replica_t *r)
{
    if (tbft_replica_has_pending_requests(r)) {
        tbft_itimer_start(&r->vtimer, vtimer_period(r));
    } else {
        tbft_itimer_stop(&r->vtimer);
    }
}

/* Peers whose recent Status shows them working normally in our view or a
 * later one. */
static int peers_working(const tbft_replica_t *r)
{
    int64_t now = esp_timer_get_time();
    int k = 0;
    for (int i = 0; i < r->node.num_replicas; i++) {
        if (i == r->node.node_id || r->peer_status_us[i] == 0) continue;
        if (now - r->peer_status_us[i] > 3 * r->stimer_period_us) continue;
        if (!r->peer_in_vc[i] && r->peer_view[i] >= r->node.view) k++;
    }
    return k;
}

/* Build our Prepare for (@p view, @p n, @p digest) in out_buf; returns its
 * length. */
static int32_t build_prepare(tbft_replica_t *r, tbft_view_t view, tbft_seqno_t n,
                             const tbft_digest_t *digest)
{
    tbft_prepare_rep_t *prep = (tbft_prepare_rep_t *)r->out_buf;
    prep->hdr.tag          = TBFT_MSG_PREPARE;
    prep->hdr.extra        = 0;
    prep->hdr.timestamp_us = esp_timer_get_time();
    prep->view             = view;
    prep->seqno            = n;
    prep->digest           = *digest;
    prep->id               = r->node.node_id;
    prep->_pad             = 0;
    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*prep));
    /* hdr.size is covered by the MAC: set it first */
    prep->hdr.size = tbft_msg_align((int32_t)(sizeof(*prep) + sizeof(tbft_auth_t)));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*prep), auth);
    return prep->hdr.size;
}

/* Build our Commit for (@p view, @p n, @p digest) in out_buf; returns its
 * length. */
static int32_t build_commit(tbft_replica_t *r, tbft_view_t view, tbft_seqno_t n,
                            const tbft_digest_t *digest)
{
    tbft_commit_rep_t *cm = (tbft_commit_rep_t *)r->out_buf;
    cm->hdr.tag          = TBFT_MSG_COMMIT;
    cm->hdr.extra        = 0;
    cm->hdr.timestamp_us = esp_timer_get_time();
    cm->view             = view;
    cm->seqno            = n;
    cm->digest           = *digest;   /* PBFT: commit must include the digest */
    cm->id               = r->node.node_id;
    cm->_pad             = 0;
    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*cm));
    cm->hdr.size = tbft_msg_align((int32_t)(sizeof(*cm) + sizeof(tbft_auth_t)));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*cm), auth);
    return cm->hdr.size;
}

/* Broadcast our Prepare for @p n (once per view). */
static void send_prepare(tbft_replica_t *r, tbft_seqno_t n)
{
    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
    if (!pp_buf) return;
    tbft_digest_t digest = ((const tbft_pre_prepare_rep_t *)pp_buf)->digest;

    int32_t len = build_prepare(r, r->node.view, n, &digest);
    if (!tbft_ar_add_my_prepare(&r->ar, n, r->out_buf, len, r->node.node_id)) return;
    tbft_node_send(&r->node, r->out_buf, (size_t)len, TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent prepare seqno=%lld", (long long)n);
}

/* Broadcast our Commit for @p n (once per view). */
static void send_commit(tbft_replica_t *r, tbft_seqno_t n)
{
    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
    if (!pp_buf) return;
    tbft_digest_t digest = ((const tbft_pre_prepare_rep_t *)pp_buf)->digest;

    int32_t len = build_commit(r, r->node.view, n, &digest);
    if (!tbft_ar_add_my_commit(&r->ar, n, r->out_buf, len, r->node.node_id)) return;
    tbft_ar_slice(&r->ar, n)->commit_sent_us = esp_timer_get_time();
    tbft_node_send(&r->node, r->out_buf, (size_t)len, TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "sent commit seqno=%lld", (long long)n);
}

/* Send our Checkpoint message for @p seqno (when we hold its digest). */
static void send_checkpoint_to(tbft_replica_t *r, tbft_seqno_t seqno, int dest)
{
    tbft_digest_t digest;
    if (seqno <= 0 || !tbft_state_get_checkpoint_digest(&r->state, seqno, &digest)) return;
    tbft_checkpoint_rep_t *ckpt = (tbft_checkpoint_rep_t *)r->out_buf;
    ckpt->hdr.tag          = TBFT_MSG_CHECKPOINT;
    ckpt->hdr.extra        = 0;
    ckpt->hdr.timestamp_us = esp_timer_get_time();
    ckpt->seqno            = seqno;
    ckpt->digest           = digest;
    ckpt->id               = r->node.node_id;
    ckpt->_pad             = 0;
    /* hdr.size is covered by the MAC: set it first */
    ckpt->hdr.size = tbft_msg_align((int32_t)(sizeof(*ckpt) + sizeof(tbft_auth_t)));
    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*ckpt));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*ckpt), auth);
    tbft_node_send(&r->node, r->out_buf, (size_t)ckpt->hdr.size, dest);
}

/* Broadcast our Status: view, what we executed, and whether we are in a
 * view change.  Peers answer it with what we are missing. */
static void send_status(tbft_replica_t *r, int16_t flags)
{
    tbft_status_rep_t *st = (tbft_status_rep_t *)r->out_buf;
    st->hdr.tag          = TBFT_MSG_STATUS;
    st->hdr.extra        = (int16_t)(flags | (r->vi.in_progress ? TBFT_STATUS_IN_VC : 0));
    st->hdr.size         = tbft_msg_align((int32_t)sizeof(*st));
    st->hdr.timestamp_us = esp_timer_get_time();
    st->view             = r->node.view;
    st->last_stable      = r->last_stable;
    st->last_prepared    = r->last_prepared;
    st->last_executed    = r->last_executed;
    st->id               = r->node.node_id;
    st->_pad             = 0;
    tbft_node_send(&r->node, r->out_buf, (size_t)st->hdr.size, TBFT_ALL_REPLICAS);
}

/* Execution is stuck on a sequence number whose messages we lack: ask the
 * others for them now rather than at the next periodic Status. */
static void ask_for_missing(tbft_replica_t *r)
{
    int64_t now = esp_timer_get_time();
    if (r->last_ask_us != 0 && now - r->last_ask_us < TBFT_ASK_INTERVAL_US) return;
    r->last_ask_us = now;
    ESP_LOGI(TAG, "asking for missing messages above seqno=%lld", (long long)r->last_executed);
    send_status(r, TBFT_STATUS_WANT);
}

/* Give @p peer what we still hold for seqnos (from, to] of the current view
 * (also below our window, until those slots are reused): our commit, the
 * pre-prepare exactly as the primary sent it (its MAC still verifies), and
 * our prepare. */
static void retransmit_to(tbft_replica_t *r, int peer, tbft_seqno_t from, tbft_seqno_t to)
{
    for (tbft_seqno_t n = from + 1; n <= to; n++) {
        const tbft_agreement_slice_t *sl = tbft_ar_peek(&r->ar, n);
        if (!sl || sl->prepared_cert.pp_len <= 0) continue;
        const tbft_pre_prepare_rep_t *pp =
            (const tbft_pre_prepare_rep_t *)sl->prepared_cert.pp_buf;
        if (pp->view != r->node.view) continue;
        tbft_digest_t digest = pp->digest;
        /* Commits first: once the peer holds 2f+1 of them it also accepts a
         * pre-prepare whose MAC no longer verifies (relabelled by a New_view). */
        if (tbft_bitmap_test(&sl->commit_cert.bmap, r->node.node_id)) {
            int32_t len = build_commit(r, pp->view, n, &digest);
            tbft_node_send(&r->node, r->out_buf, (size_t)len, peer);
            r->stat_commit_resent++;
        }
        if (pp->rset_size > 0) {   /* null requests come with the New_view */
            tbft_node_send(&r->node, sl->prepared_cert.pp_buf,
                           (size_t)sl->prepared_cert.pp_len, peer);
        }
        if (tbft_bitmap_test(&sl->prepared_cert.pc.bmap, r->node.node_id)) {
            int32_t len = build_prepare(r, pp->view, n, &digest);
            tbft_node_send(&r->node, r->out_buf, (size_t)len, peer);
        }
    }
}

/* Highest checkpoint with 2f+1 matching Checkpoint messages, or 0. */
static tbft_seqno_t newest_stable_ckpt(const tbft_replica_t *r)
{
    tbft_seqno_t best = 0;
    for (int i = 0; i < TBFT_NUM_CKPT_SLOTS; i++) {
        const tbft_ckpt_slot_t *s = &r->cr.slots[i];
        if (s->seqno > best && s->match_count >= r->cr.threshold) best = s->seqno;
    }
    return best;
}

/* Checkpoint @p seqno is certified (2f+1 matching Checkpoint messages, or
 * fixed by a New_view) and our state at it matches: move the window. */
static void install_stable(tbft_replica_t *r, tbft_seqno_t seqno)
{
    if (seqno < r->last_stable) return;
    bool advanced = seqno > r->last_stable;
    tbft_state_mark_stable(&r->state, seqno);
    r->last_stable = seqno;
    r->stable_wait_seqno = 0;
    if (r->last_prepared < seqno) r->last_prepared = seqno;
    tbft_ar_truncate(&r->ar, seqno + 1);
    tbft_cr_truncate(&r->cr, seqno);
    update_vtimer(r);
    if (advanced) ESP_LOGI(TAG, "marked stable seqno=%lld", (long long)seqno);
}

/* Fetch the state of checkpoint @p seqno, whose root digest is @p digest.
 * Throttled; a fetch already aiming at this or a later checkpoint is kept.
 * @return true if a fetch was started */
static bool start_fetch(tbft_replica_t *r, tbft_seqno_t seqno, const tbft_digest_t *digest)
{
    if (tbft_state_in_fetch(&r->state) && r->state.fetch_seqno >= seqno) return false;
    int64_t now = esp_timer_get_time();
    if (r->last_fetch_throttle_us != 0 && now - r->last_fetch_throttle_us < 500000LL) {
        return false;
    }
    r->last_fetch_throttle_us = now;
    int replier = tbft_node_primary(&r->node, r->node.view);
    if (replier == r->node.node_id) replier = (replier + 1) % r->node.num_replicas;
    tbft_replica_start_fetch(r, seqno, replier, digest);
    return true;
}

/* All blocks of the fetched checkpoint arrived: check and adopt them. */
static void finish_fetch(tbft_replica_t *r, tbft_seqno_t seqno)
{
    tbft_digest_t want;
    bool have_want = false;
    if (r->state.fetch_has_digest) {
        want = r->state.fetch_digest;
        have_want = true;
    } else {
        const tbft_digest_t *w = tbft_cr_winning_digest(&r->cr, seqno);
        if (w) {
            want = *w;
            have_want = true;
        }
    }
    if (!have_want || !tbft_digest_equal(tbft_state_root_digest(&r->state), &want)) {
        int replier = (r->state.fetch_replier + 1) % r->node.num_replicas;
        if (replier == r->node.node_id) replier = (replier + 1) % r->node.num_replicas;
        ESP_LOGW(TAG, "fetched state for seqno=%lld does not match its certified digest"
                 " - fetching again from replica %d", (long long)seqno, replier);
        tbft_replica_start_fetch(r, seqno, replier, have_want ? &want : nullptr);
        return;
    }
    tbft_state_install_fetched(&r->state, seqno);
    r->last_executed = seqno;   /* later seqnos execute again from the window */
    if (r->last_prepared < seqno) r->last_prepared = seqno;
    sync_executed_rids(r);
    install_stable(r, seqno);
    if (r->seqno <= seqno) r->seqno = seqno + 1;
    ESP_LOGW(TAG, "state transfer complete at seqno=%lld", (long long)seqno);
    tbft_replica_execute_committed(r);
    tbft_seqno_t newest = newest_stable_ckpt(r);
    if (newest > r->last_stable) tbft_replica_mark_stable(r, newest);
    /* the window moved: propose what waits in the queue */
    if (tbft_replica_is_primary(r)) tbft_replica_send_pre_prepare(r);
}

/* Validate the body of a pre-prepare: digest over the request set and the
 * non-deterministic choices, and the embedded client request with its
 * signature.  Null request sets only come from a New_view. */
static bool pp_body_valid(tbft_replica_t *r, const tbft_pre_prepare_rep_t *pp,
                          const uint8_t *msg)
{
    if (pp->rset_size <= 0) {
        ESP_LOGW(TAG, "pp: empty request set rejected");
        return false;
    }
    const uint8_t *req_set = msg + sizeof(*pp);
    tbft_digest_t computed;
    tbft_msg_digest(req_set, (size_t)pp->rset_size + (size_t)pp->non_det_size, &computed);
    if (!tbft_digest_equal(&pp->digest, &computed)) {
        ESP_LOGW(TAG, "pp: digest mismatch");
        return false;
    }
    /* Re-verify the embedded client RSA signature: a Byzantine primary must
     * not be able to inject requests the client never signed. */
    if ((size_t)pp->rset_size < sizeof(tbft_request_rep_t) + sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "pp: embedded request too short (%d)", (int)pp->rset_size);
        return false;
    }
    const tbft_request_rep_t *ereq = (const tbft_request_rep_t *)req_set;
    if (ereq->hdr.tag != TBFT_MSG_REQUEST) {
        ESP_LOGW(TAG, "pp: embedded message is not a Request (tag=%d)", (int)ereq->hdr.tag);
        return false;
    }
    if (ereq->command_size < 0 ||
        (size_t)ereq->command_size >
            (size_t)pp->rset_size - sizeof(tbft_request_rep_t) - sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "pp: embedded command_size %d doesn't fit in rset_size %d",
                 (int)ereq->command_size, (int)pp->rset_size);
        return false;
    }
    if (client_index(r, ereq->cid) < 0) {
        ESP_LOGW(TAG, "pp: embedded request from non-client id %d", (int)ereq->cid);
        return false;
    }
    size_t sig_off = sizeof(tbft_request_rep_t) + (size_t)ereq->command_size;
    /* hdr+rep+cmd+sig <= rset_size; up to 7 bytes of 8-byte alignment padding */
    if (sig_off + sizeof(tbft_sig_t) > (size_t)pp->rset_size ||
        (size_t)pp->rset_size - sig_off - sizeof(tbft_sig_t) > 7) {
        ESP_LOGW(TAG, "pp: embedded request size mismatch (sig_off=%zu, rset=%d)",
                 sig_off, (int)pp->rset_size);
        return false;
    }
    const tbft_sig_t *ereq_sig = (const tbft_sig_t *)(req_set + sig_off);
    if (!tbft_node_verify_sig(&r->node, (tbft_node_id_t)ereq->cid, ereq, sig_off, ereq_sig)) {
        ESP_LOGW(TAG, "pp: embedded request signature invalid from cid=%d", (int)ereq->cid);
        return false;
    }
    if (ereq->rid > r->last_received_rid[ereq->cid]) {
        r->last_received_rid[ereq->cid] = ereq->rid;
    }
    return true;
}

/* After a pre-prepare, prepare or commit for @p n: record the request as
 * prepared in this view, send our commit once prepared, execute once
 * committed. */
static void check_prepared_committed(tbft_replica_t *r, tbft_seqno_t n)
{
    if (!tbft_ar_in_range(&r->ar, n)) return;
    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, n);
    bool prepared  = tbft_ar_prepared(&r->ar, n);
    bool committed = tbft_ar_committed(&r->ar, n);
    if (prepared && n > r->last_prepared) r->last_prepared = n;

    int pp_len = 0;
    const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
    if (pp_buf) {
        const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)pp_buf;
        bool certified = prepared;
        if (!certified && committed) {
            const tbft_commit_rep_t *win =
                (const tbft_commit_rep_t *)tbft_commit_cert_cvalue(&sl->commit_cert);
            certified = win && tbft_digest_equal(&win->digest, &pp->digest);
        }
        if (certified && r->node.view >= sl->prep_view) {
            sl->prep_view   = r->node.view;   /* reported in our View_change */
            sl->prep_digest = pp->digest;
        }
    }

    /* After sending a View_change a replica sends no prepare or commit until
     * the next view starts: its View_change must stay a complete account of
     * what it prepared.  It still executes what others commit. */
    if (prepared && !r->vi.in_progress &&
        !tbft_bitmap_test(&sl->commit_cert.bmap, r->node.node_id)) {
        send_commit(r, n);
        committed = tbft_ar_committed(&r->ar, n);
    }
    if (committed) {
        tbft_replica_execute_committed(r);
    }
}

/* Execute the request of @p pp at sequence number @p n; null requests are
 * no-ops and a request its client already ran is not run again. */
static void execute_one(tbft_replica_t *r, tbft_seqno_t n, const tbft_pre_prepare_rep_t *pp)
{
    int req_len = pp->rset_size;
    if (req_len == 0) {
        ESP_LOGI(TAG, "execute: seqno=%lld null request", (long long)n);
        return;
    }
    const uint8_t *req_bytes = (const uint8_t *)pp + sizeof(*pp);
    const uint8_t *ndet      = req_bytes + req_len;
    const tbft_request_rep_t *req = (const tbft_request_rep_t *)req_bytes;
    int idx = req_len >= (int)sizeof(*req) ? client_index(r, req->cid) : -1;
    if (idx < 0) {
        ESP_LOGW(TAG, "execute: seqno=%lld malformed request - no-op", (long long)n);
        return;
    }
    tbft_client_entry_t *e = client_entry(r, idx);
    if (req->rid <= e->last_rid) {
        ESP_LOGI(TAG, "execute: seqno=%lld cid=%d rid=%llu ran already - no-op",
                 (long long)n, req->cid, (unsigned long long)req->rid);
        return;
    }

    ESP_LOGI(TAG, "execute: calling exec_cb for seqno=%lld req_len=%d",
             (long long)n, req_len);
    memset(r->out_buf, 0, sizeof(r->out_buf));
    uint8_t *rep_payload = r->out_buf + sizeof(tbft_reply_rep_t);
    int      rep_len     = 0;
    bool     ro          = (req->hdr.extra & TBFT_REQUEST_RO_FLAG) != 0;
    int rc = r->exec_cb
        ? r->exec_cb(req_bytes, req_len, rep_payload, &rep_len,
                     (void *)ndet, pp->non_det_size, req->cid, ro)
        : -1;

    /* Record the outcome in the replicated client table (identical on every
     * correct replica), then answer the client from it. */
    client_entry_touch(r, idx);
    e->last_rid = req->rid;
    e->seqno    = n;
    if (rc != 0 || rep_len < 0 || rep_len > TBFT_MAX_REPLY_SIZE) {
        if (rc == 0) {
            ESP_LOGE(TAG, "execute: reply of %d bytes exceeds TBFT_MAX_REPLY_SIZE", rep_len);
        } else {
            ESP_LOGW(TAG, "exec_cb rejected seqno=%lld - no reply", (long long)n);
        }
        e->reply_len = -1;
        memset(e->reply, 0, sizeof(e->reply));
    } else {
        e->reply_len = rep_len;
        memcpy(e->reply, rep_payload, (size_t)rep_len);
        memset(e->reply + rep_len, 0, sizeof(e->reply) - (size_t)rep_len);
    }
    if (req->rid > r->last_executed_rid[req->cid]) {
        r->last_executed_rid[req->cid] = req->rid;
    }
    send_reply(r, req->cid, e, true);
}

/* Checkpoint at @p n: record the state digest and broadcast our Checkpoint. */
static void take_checkpoint(tbft_replica_t *r, tbft_seqno_t n)
{
    tbft_state_checkpoint(&r->state, n);

    tbft_checkpoint_rep_t *ckpt = (tbft_checkpoint_rep_t *)r->out_buf;
    ckpt->hdr.tag          = TBFT_MSG_CHECKPOINT;
    ckpt->hdr.extra        = 0;
    ckpt->hdr.timestamp_us = esp_timer_get_time();
    ckpt->seqno            = n;
    ckpt->digest           = *tbft_state_root_digest(&r->state);
    ckpt->id               = r->node.node_id;
    ckpt->_pad             = 0;
    tbft_auth_t *auth = (tbft_auth_t *)(r->out_buf + sizeof(*ckpt));
    /* hdr.size is covered by the MAC: set it first */
    ckpt->hdr.size = tbft_msg_align((int32_t)(sizeof(*ckpt) + sizeof(tbft_auth_t)));
    tbft_node_gen_auth(&r->node, r->out_buf, sizeof(*ckpt), auth);

    /* Store our own checkpoint before the broadcast (a spoofed message with
     * our id must not bypass authentication in handle_checkpoint). */
    bool self_stable = tbft_cr_store(&r->cr, n, r->node.node_id, r->out_buf, ckpt->hdr.size);
    tbft_node_send(&r->node, r->out_buf, (size_t)ckpt->hdr.size, TBFT_ALL_REPLICAS);
    ESP_LOGI(TAG, "broadcast checkpoint at seqno=%lld", (long long)n);
    if (self_stable) {
        tbft_replica_mark_stable(r, n);
    }
}

/* View-change timer expired. */
static void handle_vtimer(tbft_replica_t *r)
{
    int keys_ok = count_fresh_keys(r);
    if (keys_ok < r->node.threshold - 1) {
        /* Without the keys, prepares and commits cannot be verified, so a
         * view change now would be spurious. */
        ESP_LOGD(TAG, "view-change suppressed: keys=%d/%d", keys_ok, r->node.threshold - 1);
        update_vtimer(r);
        return;
    }
    if (r->vi.in_progress && peers_working(r) >= r->node.max_faulty + 1) {
        /* f+1 others still work in the view we want to leave: wait until
         * they time out too (they will ask for the same view) rather than
         * moving further away.  Repeat our View_change in case it was lost. */
        int vc_len = 0;
        const uint8_t *vc = tbft_sr_load_vc(&r->sr, r->node.node_id, &vc_len);
        if (vc) tbft_node_send(&r->node, vc, (size_t)vc_len, TBFT_ALL_REPLICAS);
        tbft_itimer_start(&r->vtimer, vtimer_period(r));
        ESP_LOGI(TAG, "view change to %lld waits: %d replicas still work in view %lld",
                 (long long)r->vi.target_view, peers_working(r), (long long)r->node.view);
        return;
    }
    ESP_LOGW(TAG, "view-change timeout in view %lld (keys=%d/%d)",
             (long long)r->node.view, keys_ok, r->node.threshold - 1);
    tbft_replica_send_view_change(r);
}

/* The current fetch got no answer in time: aim at the newest certified
 * checkpoint, or ask another replica. */
static void fetch_timeout(tbft_replica_t *r)
{
    tbft_seqno_t newest = newest_stable_ckpt(r);
    if (newest > r->state.fetch_seqno) {
        ESP_LOGW(TAG, "fetch of seqno=%lld timed out - fetching stable checkpoint %lld",
                 (long long)r->state.fetch_seqno, (long long)newest);
        tbft_digest_t target = *tbft_cr_winning_digest(&r->cr, newest);
        r->last_fetch_throttle_us = 0;
        (void)start_fetch(r, newest, &target);
        return;
    }
    int replier = (r->state.fetch_replier + 1) % r->node.num_replicas;
    if (replier == r->node.node_id) replier = (replier + 1) % r->node.num_replicas;
    ESP_LOGW(TAG, "fetch timeout at seqno=%lld - asking replica %d",
             (long long)r->state.fetch_seqno, replier);
    tbft_digest_t target = r->state.fetch_digest;
    bool has_target = r->state.fetch_has_digest;
    tbft_replica_start_fetch(r, r->state.fetch_seqno, replier, has_target ? &target : nullptr);
}

void tbft_replica_print_stats(const tbft_replica_t *r)
{
    tbft_transport_stats_t t;
    tbft_transport_get_stats(r->node.transport, &t);
    ESP_LOGW(TAG, "TBFT_STATS id=%d view=%lld last_exec=%lld last_stable=%lld "
             "executed=%lu commit_resent=%lu vc_sent=%lu new_view=%lu fetch=%lu "
             "tx=%lu tx_frames=%lu tx_fail=%lu tx_qfull=%lu rx=%lu rx_drop=%lu rx_frag_drop=%lu "
             "tx_pp=%lu tx_prep=%lu tx_commit=%lu rx_pp=%lu rx_prep=%lu rx_commit=%lu "
             "heap=%lu heap_min=%lu",
             r->node.node_id, (long long)r->node.view,
             (long long)r->last_executed, (long long)r->last_stable,
             (unsigned long)r->stat_executed, (unsigned long)r->stat_commit_resent,
             (unsigned long)r->stat_view_change_sent, (unsigned long)r->stat_new_view,
             (unsigned long)r->stat_fetch_started,
             (unsigned long)t.tx_msgs, (unsigned long)t.tx_frames,
             (unsigned long)t.tx_fail,
             (unsigned long)t.tx_queue_full, (unsigned long)t.rx_msgs,
             (unsigned long)t.rx_drop, (unsigned long)t.rx_frag_drop,
             (unsigned long)t.tx_by_tag[TBFT_MSG_PRE_PREPARE],
             (unsigned long)t.tx_by_tag[TBFT_MSG_PREPARE],
             (unsigned long)t.tx_by_tag[TBFT_MSG_COMMIT],
             (unsigned long)t.rx_by_tag[TBFT_MSG_PRE_PREPARE],
             (unsigned long)t.rx_by_tag[TBFT_MSG_PREPARE],
             (unsigned long)t.rx_by_tag[TBFT_MSG_COMMIT],
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
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
     * if even a single node is offline or fails to complete key exchange.
     * Our New_key asks the others to answer with theirs, so a replica that
     * boots into a running cluster gets its keys within seconds. */
    {
        const int needed = 2 * r->node.max_faulty;
        TickType_t key_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        TickType_t last_nk = xTaskGetTickCount();
        ESP_LOGI(TAG, "waiting for at least %d HMAC keys to start consensus (timeout=60s)", needed);
        while (xTaskGetTickCount() < key_deadline) {
            int keys_ok = count_fresh_keys(r);
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
        ESP_LOGI(TAG, "key barrier complete: %d/%d keys ready",
                 count_fresh_keys(r), r->node.num_replicas - 1);
    }
    /* Subscribe this task to the task watchdog so we can periodically feed it.
     * The default timeout is CONFIG_ESP_TASK_WDT_TIMEOUT_S (typically 5s). */
#if CONFIG_ESP_TASK_WDT_EN
    esp_err_t err = esp_task_wdt_add(nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to subscribe to task WDT: %d", err);
    }
#endif

    TickType_t last_nk_send = xTaskGetTickCount();
    TickType_t nk_period    = pdMS_TO_TICKS(5000);
#if CONFIG_TBFT_PRINT_STATS
    TickType_t last_stats = xTaskGetTickCount();
#endif

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

#if CONFIG_TBFT_PRINT_STATS
        if (xTaskGetTickCount() - last_stats >= pdMS_TO_TICKS(60000)) {
            last_stats = xTaskGetTickCount();
            tbft_replica_print_stats(r);
        }
#endif

        /* Periodically re-broadcast New_key (it tells the others which keys
         * we still lack, and they answer): every 5s below the 2f keys
         * consensus needs; with those but not all, 5s doubling up to 60s (a
         * lost New_key is recovered quickly, a replica that is down costs
         * little); every 60s with all keys. */
        {
            int keys_ok = count_fresh_keys(r);
            TickType_t interval = pdMS_TO_TICKS(5000);
            if (keys_ok >= r->node.num_replicas - 1) {
                interval  = pdMS_TO_TICKS(60000);
                nk_period = pdMS_TO_TICKS(5000);
            } else if (keys_ok >= r->node.threshold - 1) {
                interval = nk_period;
            }
            TickType_t now = xTaskGetTickCount();
            if (now - last_nk_send >= interval) {
                tbft_replica_send_new_key(r);
                last_nk_send = now;
                if (keys_ok >= r->node.threshold - 1 && nk_period < pdMS_TO_TICKS(60000)) {
                    nk_period *= 2;
                    if (nk_period > pdMS_TO_TICKS(60000)) nk_period = pdMS_TO_TICKS(60000);
                }
            }
        }

        if (r->evt_group) {
            EventBits_t bits = xEventGroupWaitBits(
                r->evt_group,
                TBFT_EVT_VTIMER | TBFT_EVT_STIMER,
                pdTRUE,   /* clear on exit */
                pdFALSE,  /* wait for any bit */
                0);   /* poll only: blocking happens in tbft_node_wait() below */
            if (bits & TBFT_EVT_VTIMER) {
                handle_vtimer(r);
            }
            if (bits & TBFT_EVT_STIMER) {
                send_status(r, 0);
                tbft_itimer_start(&r->stimer, r->stimer_period_us);
                /* requests that waited for room in the window */
                if (tbft_replica_is_primary(r)) tbft_replica_send_pre_prepare(r);
                /* a checkpoint the quorum certified ahead of us */
                tbft_seqno_t newest = newest_stable_ckpt(r);
                if (newest > r->last_stable) tbft_replica_mark_stable(r, newest);
            }
        }

        if (tbft_state_in_fetch(&r->state) && tbft_state_fetch_timedout(&r->state)) {
            fetch_timeout(r);
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
            /* No message available: sleep until one arrives or the idle
             * wait elapses (timer event bits are polled at the top of the
             * loop, so they are serviced within TBFT_IDLE_WAIT_MS). */
            tbft_node_wait(&r->node, TBFT_IDLE_WAIT_MS);
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

        /* Until we have the sender's HMAC session key, Prepare, Commit and
         * Checkpoint messages (HMAC-authenticated) cannot be verified: drop
         * them here instead of failing verification later.  Pre-prepares go
         * through: one re-sent by another replica may be accepted on the
         * strength of a commit quorum (see handle_pre_prepare). */
        if (hdr->tag == TBFT_MSG_PREPARE || hdr->tag == TBFT_MSG_COMMIT ||
            hdr->tag == TBFT_MSG_CHECKPOINT) {
            int sender_id = -1;
            if (hdr->tag == TBFT_MSG_PREPARE && n >= (int)sizeof(tbft_prepare_rep_t)) {
                sender_id = ((const tbft_prepare_rep_t *)r->node.recv_buf)->id;
            } else if (hdr->tag == TBFT_MSG_COMMIT && n >= (int)sizeof(tbft_commit_rep_t)) {
                sender_id = ((const tbft_commit_rep_t *)r->node.recv_buf)->id;
            } else if (hdr->tag == TBFT_MSG_CHECKPOINT &&
                       n >= (int)sizeof(tbft_checkpoint_rep_t)) {
                sender_id = ((const tbft_checkpoint_rep_t *)r->node.recv_buf)->id;
            }
            if (sender_id >= 0 && sender_id < r->node.num_replicas) {
                tbft_principal_t *p = r->node.principals[sender_id];
                if (!p || !p->keys_fresh) {
                    ESP_LOGD(TAG, "dropping tag %d from %d: key not fresh", hdr->tag, sender_id);
                    continue;
                }
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
        case TBFT_MSG_STATUS:
            handle_status(r, r->node.recv_buf, n);
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
    esp_task_wdt_delete(nullptr);
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
    int idx = client_index(r, req->cid);
    if (idx < 0) {
        ESP_LOGW(TAG, "request: id %d is not a client (num_replicas=%d, num_principals=%d)",
                 req->cid, r->node.num_replicas, r->node.num_principals);
        return;
    }

    /* Copies of a request we already know (the client broadcasts and
     * retransmits, backups forward) are settled first, without the costly
     * signature check.
     *
     * Executed already: never run it again; answer the latest one from the
     * reply cache. */
    const tbft_client_entry_t *e = client_entry(r, idx);
    if (req->rid <= e->last_rid) {
        if (req->rid == e->last_rid) {
            send_reply(r, req->cid, e, false);
        }
        return;
    }
    if (!r->vi.in_progress) {
        if (tbft_replica_is_primary(r)) {
            /* It has a sequence number: in flight.  Replicas missing its
             * pre-prepare get it through Status. */
            if (req->rid <= r->last_assigned_rid[req->cid]) {
                ESP_LOGD(TAG, "request in flight: cid=%d rid=%llu", req->cid,
                         (unsigned long long)req->rid);
                return;
            }
        } else if (req->rid <= r->last_forwarded_rid[req->cid]) {
            return;   /* forwarded to the primary already; the timer runs */
        }
    }

    /* Verify request signature */
    int sig_offset = (int)(sizeof(*req) + (size_t)req->command_size);
    if (len < sig_offset + (int)sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "request: missing signature");
        return;
    }
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + sig_offset);
    if (!tbft_node_verify_sig(&r->node, (tbft_node_id_t)req->cid,
                              req, (size_t)sig_offset, sig)) {
        ESP_LOGW(TAG, "request: signature verification failed from client %d",
                 req->cid);
        return;
    }
    if (req->rid > r->last_received_rid[req->cid]) {
        r->last_received_rid[req->cid] = req->rid;
    }
    /* During a view change the request waits for the next primary, which
     * gets it from the client's retransmission (the view-change timer runs). */
    if (r->vi.in_progress) return;

    /* Backups forward each request to the primary once (the client
     * broadcasts, so this is a fallback for a lost direct send) and start
     * the view-change timer, which retransmissions do not restart: a client
     * retransmitting faster than the timer must not keep a faulty primary
     * in place. */
    if (!tbft_replica_is_primary(r)) {
        int primary = tbft_node_primary(&r->node, r->node.view);
        ESP_LOGD(TAG, "forwarding request to primary %d (view=%lld)",
                 primary, (long long)r->node.view);
        tbft_node_send(&r->node, msg, (size_t)len, primary);
        r->last_forwarded_rid[req->cid] = req->rid;
        if (!tbft_itimer_is_running(&r->vtimer)) {
            ESP_LOGI(TAG, "vtimer started on backup: pending request from cid=%d rid=%llu",
                     req->cid, (unsigned long long)req->rid);
            tbft_itimer_start(&r->vtimer, vtimer_period(r));
        }
        return;
    }

    bool ro = (req->hdr.extra & TBFT_REQUEST_RO_FLAG) != 0;
    tbft_rqueue_t *q = ro ? &r->ro_rqueue : &r->rqueue;
    for (int i = 0; i < q->count; i++) {
        int qi = (q->head + i) % TBFT_RQUEUE_MAX;
        const tbft_request_rep_t *existing = (const tbft_request_rep_t *)q->entries[qi].buf;
        if (existing->cid == req->cid && existing->rid == req->rid) {
            ESP_LOGD(TAG, "request dedup: cid=%d rid=%llu", req->cid,
                     (unsigned long long)req->rid);
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

    /* Only the current view counts: a replica behind in view installs the
     * newer view from its New_view (re-sent on Status), never from a
     * pre-prepare. */
    if (pp->view != r->node.view) {
        ESP_LOGD(TAG, "pp: view %lld, current %lld - ignored",
                 (long long)pp->view, (long long)r->node.view);
        return;
    }
    if (!tbft_replica_in_window(r, pp->seqno) || !tbft_ar_in_range(&r->ar, pp->seqno)) {
        ESP_LOGD(TAG, "pp seqno %lld out of window (last_stable=%lld)",
                 (long long)pp->seqno, (long long)r->last_stable);
        return;
    }
    if (pp->seqno <= r->last_executed) return;   /* executed already */

    /* rset_size and non_det_size must be non-negative and fit the message */
    if (pp->rset_size < 0 || pp->non_det_size < 0) {
        ESP_LOGW(TAG, "pp: negative embedded size rset=%d ndet=%d",
                 (int)pp->rset_size, (int)pp->non_det_size);
        return;
    }
    if ((int64_t)sizeof(*pp) + pp->rset_size + pp->non_det_size +
        (int64_t)sizeof(tbft_auth_t) > len) {
        ESP_LOGW(TAG, "pp: embedded sizes exceed message length (have=%d)", len);
        return;
    }
    int auth_offset = (int)(sizeof(*pp) + (size_t)pp->rset_size + (size_t)pp->non_det_size);

    /* Authenticated by the primary's MAC.  A pre-prepare re-sent by another
     * replica carries the primary's original authenticator, so it verifies
     * the same way. */
    int primary = tbft_node_primary(&r->node, pp->view);
    bool mac_ok = false;
    if (primary != r->node.node_id) {
        const tbft_principal_t *pp_primary = r->node.principals[primary];
        int slot = tbft_node_auth_slot_index(&r->node, primary);
        if (pp_primary && pp_primary->keys_fresh && slot >= 0) {
            const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + auth_offset);
            mac_ok = tbft_node_verify_auth(&r->node, primary, msg, (size_t)auth_offset,
                                           &auth->slots[slot], pp->hdr.timestamp_us);
        }
    }
    /* Without it, accept only a request a quorum already fixed for this
     * seqno: the New_view's choice, or the digest of 2f+1 commits. */
    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, pp->seqno);
    bool fixed = sl->nv_expect && tbft_digest_equal(&sl->nv_digest, &pp->digest);
    if (!fixed && tbft_ar_committed(&r->ar, pp->seqno)) {
        const tbft_commit_rep_t *win =
            (const tbft_commit_rep_t *)tbft_commit_cert_cvalue(&sl->commit_cert);
        fixed = win && tbft_digest_equal(&win->digest, &pp->digest);
    }
    if (!mac_ok && !fixed) {
        ESP_LOGD(TAG, "pp: seqno=%lld neither authenticated by primary %d nor fixed"
                 " by a quorum", (long long)pp->seqno, primary);
        return;
    }
    if (sl->nv_expect && !tbft_digest_equal(&sl->nv_digest, &pp->digest)) {
        ESP_LOGW(TAG, "pp: seqno=%lld conflicts with the New_view", (long long)pp->seqno);
        return;
    }

    int have_len = 0;
    const tbft_pre_prepare_rep_t *have =
        (const tbft_pre_prepare_rep_t *)tbft_ar_load_pp(&r->ar, pp->seqno, &have_len);
    if (!have || !tbft_digest_equal(&have->digest, &pp->digest)) {
        if (have && have->view == pp->view && !fixed) {
            ESP_LOGW(TAG, "pp: conflicting pre-prepare for seqno=%lld", (long long)pp->seqno);
            return;
        }
        if (!pp_body_valid(r, pp, (const uint8_t *)msg)) return;
        if (!tbft_ar_replace_pp(&r->ar, pp->seqno, msg, len)) {
            ESP_LOGW(TAG, "pp: failed to store seqno=%lld", (long long)pp->seqno);
            return;
        }
        ESP_LOGI(TAG, "pp accepted: seqno=%lld view=%lld%s", (long long)pp->seqno,
                 (long long)pp->view, mac_ok ? "" : " (fixed by a quorum)");
    }

    /* The next seqno this replica would assign as primary stays above it */
    if (r->seqno < pp->seqno + 1) r->seqno = pp->seqno + 1;

    /* Backups prepare (the primary's pre-prepare stands for its own); not
     * after sending a View_change. */
    if (!r->vi.in_progress && !tbft_replica_is_primary(r)) {
        send_prepare(r, pp->seqno);
    }
    check_prepared_committed(r, pp->seqno);
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
        ESP_LOGD(TAG, "prepare: view %lld, current %lld - ignored",
                 (long long)prep->view, (long long)r->node.view);
        return;
    }
    if (!tbft_replica_in_window(r, prep->seqno) || !tbft_ar_in_range(&r->ar, prep->seqno)) {
        ESP_LOGD(TAG, "prepare: seqno %lld out of window", (long long)prep->seqno);
        return;
    }

    tbft_node_id_t sender = prep->id;
    if (sender < 0 || sender >= r->node.num_replicas) {
        ESP_LOGW(TAG, "prepare: invalid sender id %d", sender);
        return;
    }
    if (sender == r->node.node_id) return;
    /* PBFT: prepares come from backups; the primary's pre-prepare stands for
     * its prepare.  Counting a prepare from the primary as well would let a
     * faulty primary get two correct replicas prepared on different requests
     * for one seqno. */
    if (sender == tbft_node_primary(&r->node, prep->view)) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) {
        ESP_LOGW(TAG, "prepare: invalid auth slot for sender %d", sender);
        return;
    }
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*prep));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*prep),
                               &auth->slots[slot], prep->hdr.timestamp_us)) {
        ESP_LOGW(TAG, "prepare: MAC verification failed from %d", sender);
        return;
    }

    /* With the pre-prepare at hand, a prepare must carry its digest.  A
     * prepare that arrives before the pre-prepare is kept (backups prepare
     * as soon as they have the pre-prepare, so theirs often overtake ours):
     * the certificate tracks up to f+1 values, so f faulty replicas cannot
     * crowd out the correct one, and "prepared" needs the pre-prepare with
     * the same digest. */
    int pp_len = 0;
    const uint8_t *pp_bytes = tbft_ar_load_pp(&r->ar, prep->seqno, &pp_len);
    if (pp_bytes &&
        !tbft_digest_equal(&((const tbft_pre_prepare_rep_t *)pp_bytes)->digest, &prep->digest)) {
        ESP_LOGW(TAG, "prepare: digest mismatch from %d seqno=%lld",
                 sender, (long long)prep->seqno);
        return;
    }

    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, prep->seqno);
    if (tbft_bitmap_test(&sl->prepared_cert.pc.bmap, sender)) {
        ESP_LOGD(TAG, "prepare: duplicate from replica %d seqno=%lld",
                 sender, (long long)prep->seqno);
        return;
    }
    if (!tbft_ar_add_prepare(&r->ar, prep->seqno, msg, len, sender)) {
        ESP_LOGW(TAG, "prepare: rejected by agreement region seqno=%lld",
                 (long long)prep->seqno);
        return;
    }
    ESP_LOGI(TAG, "prepare accepted: seqno=%lld from replica %d",
             (long long)prep->seqno, sender);
    check_prepared_committed(r, prep->seqno);
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
        ESP_LOGD(TAG, "commit: view %lld, current %lld - ignored",
                 (long long)cm->view, (long long)r->node.view);
        return;
    }
    if (!tbft_replica_in_window(r, cm->seqno) || !tbft_ar_in_range(&r->ar, cm->seqno)) {
        ESP_LOGD(TAG, "commit: seqno %lld out of window", (long long)cm->seqno);
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
    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*cm));
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*cm),
                               &auth->slots[slot], cm->hdr.timestamp_us)) {
        ESP_LOGW(TAG, "commit: MAC verification failed from %d", sender);
        return;
    }

    /* With the pre-prepare at hand, a commit must carry its digest.  Without
     * it the commit is kept: the certificate tracks up to f+1 values, so f
     * faulty replicas cannot keep the correct digest from its 2f+1 quorum,
     * and execution checks the pre-prepare against the quorum's digest. */
    int pp_len = 0;
    const uint8_t *pp_bytes = tbft_ar_load_pp(&r->ar, cm->seqno, &pp_len);
    if (pp_bytes &&
        !tbft_digest_equal(&((const tbft_pre_prepare_rep_t *)pp_bytes)->digest, &cm->digest)) {
        ESP_LOGW(TAG, "commit: digest mismatch from %d seqno=%lld",
                 sender, (long long)cm->seqno);
        return;
    }

    tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, cm->seqno);
    if (tbft_bitmap_test(&sl->commit_cert.bmap, sender)) {
        ESP_LOGD(TAG, "commit: duplicate from replica %d seqno=%lld",
                 sender, (long long)cm->seqno);
        return;
    }
    if (!tbft_ar_add_commit(&r->ar, cm->seqno, msg, len, sender)) {
        ESP_LOGW(TAG, "commit: rejected by agreement region seqno=%lld",
                 (long long)cm->seqno);
        return;
    }
    ESP_LOGI(TAG, "commit accepted: seqno=%lld from replica %d",
             (long long)cm->seqno, sender);
    check_prepared_committed(r, cm->seqno);
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
    if (ckpt->seqno <= r->last_stable) return;   /* stable already */

    /* Never trust ckpt->id from the wire as proof of identity.
     * Our own checkpoint is stored when we take it, so any network message
     * claiming to be from us is ignored here. */
    tbft_node_id_t sender = ckpt->id;
    if (sender < 0 || sender >= r->node.num_replicas) return;
    if (sender == r->node.node_id) return;

    int slot = tbft_node_auth_slot_index(&r->node, sender);
    if (slot < 0) return;

    const tbft_auth_t *auth = (const tbft_auth_t *)((const uint8_t *)msg + sizeof(*ckpt));
    /* The MAC was computed over the checkpoint body only (not the auth),
     * so verify over sizeof(*ckpt), not hdr->size which includes auth. */
    if (!tbft_node_verify_auth(&r->node, sender, msg, sizeof(*ckpt),
                               &auth->slots[slot], ckpt->hdr.timestamp_us)) {
        ESP_LOGW(TAG, "checkpoint: MAC verification failed from %d", sender);
        return;
    }

    (void)tbft_cr_store(&r->cr, ckpt->seqno, sender, msg, len);
    /* Also when the message repeats one we hold (re-sent through Status): a
     * replica that is behind tries again to reach the certified state. */
    if (tbft_cr_winning_digest(&r->cr, ckpt->seqno)) {
        ESP_LOGI(TAG, "stable checkpoint at seqno=%lld", (long long)ckpt->seqno);
        tbft_replica_mark_stable(r, ckpt->seqno);
    }
}

/* --------------------------------------------------------------------------
 * View-change handler
 * -------------------------------------------------------------------------- */

/* PBFT: join a view change once f+1 other replicas ask for a view above
 * ours, aiming at the lowest of the views those f+1 ask for.  A single
 * faulty or confused replica therefore moves nobody. */
static void maybe_join_view_change(tbft_replica_t *r)
{
    tbft_view_t floor = r->vi.in_progress ? r->vi.target_view : r->node.view;
    tbft_view_t views[TBFT_MAX_NUM_REPLICAS];
    int k = 0;
    for (int i = 0; i < r->node.num_replicas; i++) {
        if (i == r->node.node_id) continue;
        if (r->vc_view_seen[i] > floor) views[k++] = r->vc_view_seen[i];
    }
    int need = r->node.max_faulty + 1;
    if (k < need) return;
    /* the need-th largest requested view */
    for (int i = 0; i < need; i++) {
        for (int j = i + 1; j < k; j++) {
            if (views[j] > views[i]) {
                tbft_view_t t = views[i];
                views[i] = views[j];
                views[j] = t;
            }
        }
    }
    ESP_LOGW(TAG, "%d replicas ask for a view change: joining view %lld",
             k, (long long)views[need - 1]);
    send_view_change_to(r, views[need - 1]);
}

void tbft_replica_handle_view_change(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_view_change_rep_t)) return;
    const tbft_view_change_rep_t *vc = (const tbft_view_change_rep_t *)msg;

    tbft_node_id_t sender_id = (tbft_node_id_t)vc->id;
    if (sender_id < 0 || sender_id >= r->node.num_replicas) return;
    if (sender_id == r->node.node_id) return;
    if (vc->v <= r->node.view) return;   /* stale */

    /* hdr.size is the body length; the RSA signature follows it. */
    int32_t body_size = vc->hdr.size;
    if (body_size < (int32_t)sizeof(tbft_view_change_rep_t) || body_size > len) {
        ESP_LOGW(TAG, "view-change: invalid body_size %d", (int)body_size);
        return;
    }
    if (len < (int)(body_size + (int)sizeof(tbft_sig_t))) {
        ESP_LOGW(TAG, "view-change: missing RSA signature from %d", sender_id);
        return;
    }
    int32_t total = body_size + (int32_t)sizeof(tbft_sig_t);
    if (total > TBFT_VC_MSG_MAX_SIZE ||
        vc->n_ckpts < 0 || vc->n_ckpts > 1 || vc->n_reqs < 0 || vc->n_reqs > TBFT_WINDOW_SIZE ||
        (int64_t)sizeof(*vc) + (int64_t)vc->n_ckpts * (int64_t)sizeof(tbft_vc_ckpt_t) +
            (int64_t)vc->n_reqs * (int64_t)sizeof(tbft_vc_req_info_t) > body_size) {
        ESP_LOGW(TAG, "view-change: malformed entry counts from %d", sender_id);
        return;
    }
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + body_size);
    if (!tbft_node_verify_sig(&r->node, sender_id, msg, (size_t)body_size, sig)) {
        ESP_LOGW(TAG, "view-change: RSA signature verification failed from %d", sender_id);
        return;
    }

    if (vc->v > r->vc_view_seen[sender_id]) {
        r->vc_view_seen[sender_id] = vc->v;
    }
    (void)tbft_vi_collect_vc(&r->vi, sender_id, msg, total);
    ESP_LOGI(TAG, "view-change from %d for view %lld (target %lld: %d/%d)",
             sender_id, (long long)vc->v, (long long)r->vi.target_view,
             r->vi.n_received, r->vi.threshold);

    maybe_join_view_change(r);
    maybe_send_new_view(r);
}

/* --------------------------------------------------------------------------
 * New-view handler
 * -------------------------------------------------------------------------- */

/* Install view nv->v as its New_view fixes it (new primary and backups
 * alike): the stable checkpoint min, and for every seqno min+1..max the
 * request to re-propose (or a null request).  Pre-prepare bodies and the
 * prepared records survive; prepares and commits of older views do not.
 * @p nv points at the stored copy in the special region. */
static void nv_install(tbft_replica_t *r, const tbft_new_view_rep_t *nv)
{
    const tbft_vc_req_info_t *o = (const tbft_vc_req_info_t *)(nv + 1);

    r->node.view        = nv->v;
    r->node.cur_primary = tbft_node_primary(&r->node, nv->v);
    tbft_replica_reset_forwarded_requests(r);
    r->vi.in_progress = false;
    tbft_vi_set_target(&r->vi, nv->v + 1);
    bool primary = tbft_replica_is_primary(r);

    /* 1. The stable checkpoint the view starts from */
    tbft_seqno_t h = nv->min;
    if (h > r->last_stable) {
        tbft_digest_t mine;
        if (r->last_executed >= h &&
            tbft_state_get_checkpoint_digest(&r->state, h, &mine) &&
            tbft_digest_equal(&mine, &nv->min_digest)) {
            install_stable(r, h);
        } else {
            /* Behind (or diverged): the window moves now and the state at h
             * comes by fetch; execution resumes once it is in place. */
            r->last_stable = h;
            if (r->last_prepared < h) r->last_prepared = h;
            tbft_ar_truncate(&r->ar, h + 1);
            tbft_cr_truncate(&r->cr, h);
            tbft_digest_t target = nv->min_digest;
            r->last_fetch_throttle_us = 0;
            (void)start_fetch(r, h, &target);
        }
    }

    /* 2. The window starts over in the new view.  Bodies outside the
     *    re-proposed range belong to requests no quorum prepared: drop them
     *    so the new primary can use those seqnos. */
    for (tbft_seqno_t n = r->ar.head; n < r->ar.head + TBFT_WINDOW_SIZE; n++) {
        tbft_ar_new_view_slot(&r->ar, n);
        if (n <= h || n > nv->max) (void)tbft_ar_replace_pp(&r->ar, n, nullptr, 0);
    }

    /* 3. The re-proposed requests, from local bodies where we have them */
    for (int i = 0; i < nv->n_prep; i++) {
        tbft_seqno_t n = h + 1 + i;
        if (!tbft_ar_in_range(&r->ar, n)) continue;
        tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, n);
        sl->nv_expect = true;
        sl->nv_digest = o[i].digest;

        if (o[i].last_view == TBFT_NV_NULL || digest_is_zero(&o[i].digest)) {
            tbft_pre_prepare_rep_t null_pp = {};
            null_pp.hdr.tag  = TBFT_MSG_PRE_PREPARE;
            null_pp.hdr.size = (int32_t)sizeof(null_pp);
            null_pp.view     = nv->v;
            null_pp.seqno    = n;
            tbft_digest_zero(&null_pp.digest);
            (void)tbft_ar_replace_pp(&r->ar, n, &null_pp, (int)sizeof(null_pp));
        } else {
            int pp_len = 0;
            uint8_t *pp_buf = (uint8_t *)tbft_ar_load_pp(&r->ar, n, &pp_len);
            tbft_pre_prepare_rep_t *have = (tbft_pre_prepare_rep_t *)pp_buf;
            if (have && tbft_digest_equal(&have->digest, &o[i].digest)) {
                have->view = nv->v;   /* the same request, pre-prepared anew */
            } else {
                (void)tbft_ar_replace_pp(&r->ar, n, nullptr, 0);   /* comes via Status */
            }
        }
        if (!primary) send_prepare(r, n);
    }

    /* 4. Sequence numbers and requests the new view already orders */
    tbft_seqno_t top = nv->max > h ? nv->max : h;
    if (r->last_executed > top) top = r->last_executed;
    r->seqno = top + 1;
    for (int cid = 0; cid < r->node.num_principals; cid++) {
        r->last_assigned_rid[cid] = r->last_executed_rid[cid];
    }
    sync_executed_rids(r);
    for (int i = 0; i < nv->n_prep; i++) {
        int pp_len = 0;
        const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, h + 1 + i, &pp_len);
        const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)pp_buf;
        if (!pp || pp->rset_size < (int32_t)sizeof(tbft_request_rep_t)) continue;
        const tbft_request_rep_t *rq = (const tbft_request_rep_t *)(pp_buf + sizeof(*pp));
        if (client_index(r, rq->cid) >= 0 && rq->rid > r->last_assigned_rid[rq->cid]) {
            r->last_assigned_rid[rq->cid] = rq->rid;
        }
    }

    r->stat_new_view++;
    update_vtimer(r);
    ESP_LOGW(TAG, "installed view %lld, primary=%d (stable=%lld, re-proposed %d)",
             (long long)nv->v, r->node.cur_primary, (long long)h, (int)nv->n_prep);

    if (primary) {
        tbft_replica_send_pre_prepare(r);
    }
}

/* New primary holding 2f+1 View_change messages for its view: decide what
 * the view re-proposes and send New_view. */
static void maybe_send_new_view(tbft_replica_t *r)
{
    if (!r->vi.in_progress) return;
    tbft_view_t v = r->vi.target_view;
    if (tbft_node_primary(&r->node, v) != r->node.node_id) return;
    if (!tbft_vi_has_quorum(&r->vi)) return;

    /* h = the highest stable checkpoint among the view-changes */
    tbft_seqno_t  h = 0;
    tbft_digest_t hd;
    tbft_digest_zero(&hd);
    for (int i = 0; i < r->node.num_replicas; i++) {
        if (!r->vi.received[i]) continue;
        int vc_len = 0;
        const uint8_t *m = tbft_sr_load_vc(&r->sr, i, &vc_len);
        if (!m) continue;
        const tbft_view_change_rep_t *vc = (const tbft_view_change_rep_t *)m;
        if (vc->ls <= h || vc->n_ckpts < 1) continue;
        const tbft_vc_ckpt_t *ck = (const tbft_vc_ckpt_t *)(m + sizeof(*vc));
        if (ck->seqno != vc->ls) continue;
        h  = vc->ls;
        hd = ck->digest;
    }

    /* For each seqno above h: the request prepared in the highest view */
    struct { bool set; tbft_view_t view; tbft_digest_t digest; } best[TBFT_WINDOW_SIZE] = {};
    tbft_seqno_t max_n = h;
    for (int i = 0; i < r->node.num_replicas; i++) {
        if (!r->vi.received[i]) continue;
        int vc_len = 0;
        const uint8_t *m = tbft_sr_load_vc(&r->sr, i, &vc_len);
        if (!m) continue;
        const tbft_view_change_rep_t *vc = (const tbft_view_change_rep_t *)m;
        const tbft_vc_req_info_t *reqs = (const tbft_vc_req_info_t *)
            (m + sizeof(*vc) + (size_t)vc->n_ckpts * sizeof(tbft_vc_ckpt_t));
        for (int j = 0; j < vc->n_reqs; j++) {
            tbft_seqno_t s = reqs[j].seqno;
            if (s <= h || s > h + TBFT_WINDOW_SIZE) continue;
            int k = (int)(s - h - 1);
            if (!best[k].set || reqs[j].last_view > best[k].view) {
                best[k].set    = true;
                best[k].view   = reqs[j].last_view;
                best[k].digest = reqs[j].digest;
            }
            if (s > max_n) max_n = s;
        }
    }

    tbft_new_view_rep_t *nv = (tbft_new_view_rep_t *)r->out_buf;
    memset(r->out_buf, 0, sizeof(r->out_buf));
    nv->hdr.tag          = TBFT_MSG_NEW_VIEW;
    nv->hdr.timestamp_us = esp_timer_get_time();
    nv->v          = v;
    nv->min        = h;
    nv->max        = max_n;
    nv->n_prep     = (int32_t)(max_n - h);
    nv->has_sig    = 1;
    nv->min_digest = hd;
    tbft_vc_req_info_t *o = (tbft_vc_req_info_t *)(r->out_buf + sizeof(*nv));
    for (int i = 0; i < nv->n_prep; i++) {
        o[i].seqno     = h + 1 + i;
        o[i].last_view = best[i].set ? best[i].view : TBFT_NV_NULL;
        if (best[i].set) {
            o[i].digest = best[i].digest;
        } else {
            tbft_digest_zero(&o[i].digest);
        }
    }
    int32_t body_size = tbft_msg_align((int32_t)(sizeof(*nv) +
                                       (size_t)nv->n_prep * sizeof(tbft_vc_req_info_t)));
    nv->hdr.size = body_size;
    if ((size_t)body_size + sizeof(tbft_sig_t) > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "new-view: out_buf too small");
        return;
    }
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + body_size);
    if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)body_size, sig) != 0) {
        ESP_LOGE(TAG, "new-view: failed to sign; refusing to send");
        return;
    }
    int32_t total = body_size + (int32_t)sizeof(tbft_sig_t);
    /* Kept: re-sent to replicas still in an older view (see handle_status) */
    if (!tbft_sr_store_nv(&r->sr, r->out_buf, total)) {
        ESP_LOGE(TAG, "new-view: cannot store (%d bytes)", (int)total);
        return;
    }
    tbft_node_send(&r->node, r->out_buf, (size_t)total, TBFT_ALL_REPLICAS);
    ESP_LOGW(TAG, "sent new-view v=%lld min=%lld max=%lld",
             (long long)v, (long long)h, (long long)max_n);

    int nv_len = 0;
    nv_install(r, (const tbft_new_view_rep_t *)tbft_sr_load_nv(&r->sr, &nv_len));
}

void tbft_replica_handle_new_view(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_new_view_rep_t)) return;
    const tbft_new_view_rep_t *nv = (const tbft_new_view_rep_t *)msg;
    /* PBFT: a replica that sent a View_change for view t accepts New_view
     * for t or later only; otherwise any view above the current one. */
    tbft_view_t lowest = r->vi.in_progress ? r->vi.target_view : r->node.view + 1;
    if (nv->v < lowest) return;

    /* Signature of the new primary over the body (hdr.size bytes) is
     * mandatory; the signature follows the body. */
    if (!nv->has_sig) {
        ESP_LOGW(TAG, "new-view: unsigned message rejected (v=%lld)", (long long)nv->v);
        return;
    }
    int32_t body_size = nv->hdr.size;
    if (body_size < (int32_t)sizeof(tbft_new_view_rep_t) || body_size > len ||
        len < body_size + (int)sizeof(tbft_sig_t)) {
        ESP_LOGW(TAG, "new-view: invalid size (body=%d len=%d)", (int)body_size, len);
        return;
    }
    if (nv->min < 0 || nv->max < nv->min || nv->n_prep != nv->max - nv->min ||
        nv->n_prep > TBFT_WINDOW_SIZE ||
        (size_t)body_size < sizeof(*nv) + (size_t)nv->n_prep * sizeof(tbft_vc_req_info_t)) {
        ESP_LOGW(TAG, "new-view: malformed (min=%lld max=%lld n_prep=%d)",
                 (long long)nv->min, (long long)nv->max, (int)nv->n_prep);
        return;
    }
    const tbft_vc_req_info_t *o = (const tbft_vc_req_info_t *)(nv + 1);
    for (int i = 0; i < nv->n_prep; i++) {
        if (o[i].seqno != nv->min + 1 + i) {
            ESP_LOGW(TAG, "new-view: entry %d has seqno %lld", i, (long long)o[i].seqno);
            return;
        }
    }
    int new_primary = tbft_node_primary(&r->node, nv->v);
    const tbft_sig_t *sig = (const tbft_sig_t *)((const uint8_t *)msg + body_size);
    if (!tbft_node_verify_sig(&r->node, new_primary, msg, (size_t)body_size, sig)) {
        ESP_LOGW(TAG, "new-view: signature verification failed (v=%lld primary=%d)",
                 (long long)nv->v, new_primary);
        return;
    }

    /* A signed New_view is accepted without holding 2f+1 View_change
     * messages here: a replica that missed some, or booted late, must still
     * be able to follow the view.  (Known limit: its choices are not checked
     * against the View_change messages, so a faulty new primary is trusted.) */
    int total = body_size + (int)sizeof(tbft_sig_t);
    if (!tbft_sr_store_nv(&r->sr, msg, total)) {
        ESP_LOGW(TAG, "new-view: cannot store (%d bytes)", total);
        return;
    }
    int nv_len = 0;
    nv_install(r, (const tbft_new_view_rep_t *)tbft_sr_load_nv(&r->sr, &nv_len));
}

/* --------------------------------------------------------------------------
 * Status handler
 * --------------------------------------------------------------------------
 * Status only triggers retransmission; it never changes this replica's view
 * or state (it carries no authenticator, and one replica's claim must move
 * nobody).  A replica behind in view gets the New_view of ours; one that
 * still lacks what we had a Status period ago gets it: our Checkpoint
 * message when it is behind our stable checkpoint (2f+1 of those make it
 * fetch the state) and the commits, pre-prepares and prepares we hold above
 * that.
 * -------------------------------------------------------------------------- */

static constexpr int64_t TBFT_RETX_INTERVAL_US = 500'000;

static void handle_status(tbft_replica_t *r, const void *msg, int len)
{
    if (len < (int)sizeof(tbft_status_rep_t)) return;
    const tbft_status_rep_t *st = (const tbft_status_rep_t *)msg;
    int p = st->id;
    if (p < 0 || p >= r->node.num_replicas || p == r->node.node_id) return;

    int64_t now = esp_timer_get_time();
    /* Compare with what we had when this replica's previous Status came: a
     * Status is a snapshot, and a request in flight when it was taken is not
     * a gap.  A replica we have not heard from lately (it just booted), or
     * one that says it is stuck, is compared with what we have now. */
    bool known = r->peer_status_us[p] != 0 &&
                 now - r->peer_status_us[p] <= 3 * r->stimer_period_us &&
                 !(st->hdr.extra & TBFT_STATUS_WANT);
    /* the highest seqno we executed, prepared or hold a pre-prepare for */
    tbft_seqno_t my_top = r->last_executed > r->last_prepared ? r->last_executed
                                                              : r->last_prepared;
    if (r->seqno - 1 > my_top) my_top = r->seqno - 1;
    tbft_seqno_t seen_top    = known ? r->peer_seen_top[p]    : my_top;
    tbft_seqno_t seen_stable = known ? r->peer_seen_stable[p] : r->last_stable;
    r->peer_seen_top[p]    = my_top;
    r->peer_seen_stable[p] = r->last_stable;
    r->peer_view[p]        = st->view;
    r->peer_in_vc[p]       = (st->hdr.extra & TBFT_STATUS_IN_VC) != 0;
    r->peer_status_us[p]   = now;

    if (now - r->retx_us[p] < TBFT_RETX_INTERVAL_US) return;

    if (st->view < r->node.view) {
        /* Behind in view: the signed New_view of our view lets it install it. */
        int nv_len = 0;
        const uint8_t *nv = tbft_sr_load_nv(&r->sr, &nv_len);
        if (nv && ((const tbft_new_view_rep_t *)nv)->v == r->node.view) {
            r->retx_us[p] = now;
            tbft_node_send(&r->node, nv, (size_t)nv_len, p);
            ESP_LOGI(TAG, "re-sent new-view %lld to replica %d (in view %lld)",
                     (long long)r->node.view, p, (long long)st->view);
        }
        return;
    }
    if (st->view > r->node.view || r->vi.in_progress || tbft_state_in_fetch(&r->state)) {
        return;
    }

    if (st->last_stable < seen_stable) {
        r->retx_us[p] = now;
        send_checkpoint_to(r, r->last_stable, p);
    }
    tbft_seqno_t from = st->last_executed;
    if (from < r->ar.head - TBFT_WINDOW_SIZE - 1) from = r->ar.head - TBFT_WINDOW_SIZE - 1;
    if (st->last_executed < seen_top && from < my_top) {
        r->retx_us[p] = now;
        ESP_LOGI(TAG, "replica %d is behind (executed %lld): re-sending %lld..%lld",
                 p, (long long)st->last_executed, (long long)from + 1, (long long)my_top);
        retransmit_to(r, p, from, my_top);
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
    /* Serve only the checkpoint whose image we keep, so the fetcher gets
     * exactly the state that checkpoint's digest covers; others are left to
     * a replica that has them (the fetcher moves on after a timeout). */
    if (!tbft_state_can_serve(&r->state, fetch->c)) {
        ESP_LOGD(TAG, "fetch: cannot serve seqno=%lld (image at %lld)",
                 (long long)fetch->c, (long long)r->state.image_seqno);
        return;
    }

    int level = fetch->level;
    int index = fetch->index;
    /* Non-negative before any array indexing (a negative value would pass
     * the upper-bound checks below). */
    if (level < 0 || index < 0) {
        ESP_LOGW(TAG, "fetch: negative level=%d or index=%d", level, index);
        return;
    }
    if (level >= r->state.ptree.dims.p_levels) return;

    if (level == r->state.ptree.dims.p_levels - 1) {
        /* Data message with the requested block */
        if (index >= r->state.num_blocks) return;
        size_t needed = sizeof(tbft_data_rep_t) + (size_t)TBFT_BLOCK_SIZE;
        if (needed > sizeof(r->out_buf)) {
            ESP_LOGE(TAG, "handle_fetch: Data response too large for out_buf (%zu > %zu)",
                     needed, sizeof(r->out_buf));
            return;
        }
        tbft_data_rep_t *data_rep = (tbft_data_rep_t *)r->out_buf;
        data_rep->hdr.tag     = TBFT_MSG_DATA;
        data_rep->hdr.extra   = 0;
        data_rep->seqno       = fetch->c;
        data_rep->block_index = index;
        data_rep->_pad        = 0;
        data_rep->digest      = r->state.ptree.ptree[level][index].digest;
        memcpy(r->out_buf + sizeof(*data_rep), tbft_state_image_block(&r->state, index),
               TBFT_BLOCK_SIZE);
        data_rep->hdr.size = tbft_msg_align((int32_t)(sizeof(*data_rep) + TBFT_BLOCK_SIZE));
        tbft_node_send(&r->node, r->out_buf, (size_t)data_rep->hdr.size, fetch->id);
        return;
    }

    int pchildren   = r->state.ptree.dims.p_children;
    int n_children  = tbft_ptree_nodes_at_level(level + 1, pchildren);
    int first_child = index * pchildren;
    int count       = pchildren;
    if (first_child + count > n_children) count = n_children - first_child;
    if (count <= 0) return;

    size_t needed = sizeof(tbft_meta_data_rep_t) + (size_t)count * sizeof(tbft_part_info_t);
    if (needed > sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "handle_fetch: Meta_data response too large for out_buf (%zu > %zu)",
                 needed, sizeof(r->out_buf));
        return;
    }
    tbft_meta_data_rep_t *md = (tbft_meta_data_rep_t *)r->out_buf;
    md->hdr.tag   = TBFT_MSG_META_DATA;
    md->hdr.extra = 0;
    md->seqno     = fetch->c;
    md->level     = level;
    md->index     = index;
    md->n_parts   = count;
    md->_pad      = 0;
    tbft_part_info_t *parts = (tbft_part_info_t *)(r->out_buf + sizeof(*md));
    for (int c = 0; c < count; c++) {
        int cidx = first_child + c;
        parts[c].digest  = r->state.ptree.ptree[level + 1][cidx].digest;
        parts[c].version = (int32_t)r->state.ptree.ptree[level + 1][cidx].version;
    }
    md->hdr.size = tbft_msg_align((int32_t)needed);
    tbft_node_send(&r->node, r->out_buf, (size_t)md->hdr.size, fetch->id);
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
            tbft_seqno_t seqno = r->state.fetch_seqno;
            tbft_state_fetch_complete(&r->state);
            ESP_LOGI(TAG, "state fetch completed for seqno=%lld", (long long)seqno);
            finish_fetch(r, seqno);
            return;
        }
    }

    /* Send next fetch requests */
    int level, index;
    while (tbft_state_next_fetch_req(&r->state, &level, &index)) {
        tbft_fetch_rep_t *fr = (tbft_fetch_rep_t *)r->out_buf;
        fr->hdr.tag     = TBFT_MSG_FETCH;
        fr->hdr.extra   = 0;
        fr->last_stable = r->last_stable;
        fr->c           = r->state.fetch_seqno;
        fr->level       = level;
        fr->index       = index;
        fr->id          = r->node.node_id;
        fr->hdr.size    = tbft_msg_align((int32_t)sizeof(*fr));
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
        ESP_LOGI(TAG, "state fetch completed for seqno=%lld", (long long)fetch_seqno);
        finish_fetch(r, fetch_seqno);
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
    if (count_fresh_keys(r) < r->node.threshold - 1) return;

    /* Assign a sequence number to every queued request the window has room
     * for.  A request already ordered (in flight or executed) is dropped:
     * the client broadcasts and retransmits, and backups forward. */
    while (tbft_replica_in_window(r, r->seqno)) {
        tbft_rqueue_t *src_queue = &r->rqueue;
        tbft_rqueue_entry_t *req = rqueue_front(src_queue);
        if (!req) {
            src_queue = &r->ro_rqueue;
            req = rqueue_front(src_queue);
        }
        if (!req) break;

        const tbft_request_rep_t *rr = (const tbft_request_rep_t *)req->buf;
        int idx = client_index(r, rr->cid);
        if (idx < 0 || rr->rid <= r->last_assigned_rid[rr->cid] ||
            rr->rid <= client_entry(r, idx)->last_rid) {
            rqueue_pop(src_queue);
            continue;
        }

        /* Compute non-deterministic choices */
        int ndet_len = 0;
        if (r->comp_ndet_cb) {
            r->comp_ndet_cb(r->seqno, r->ndet_buf, &ndet_len, r->ndet_max_len);
        }
        if (ndet_len < 0 || ndet_len > r->ndet_max_len) {
            ESP_LOGE(TAG, "pre-prepare: invalid non-deterministic data length %d", ndet_len);
            ndet_len = 0;
        }

        /* Guard against buffer overflow before building the message */
        size_t needed = sizeof(tbft_pre_prepare_rep_t) + (size_t)req->len
                      + (size_t)ndet_len + sizeof(tbft_auth_t);
        size_t aligned_needed = (needed + 7u) & ~(size_t)7u;
        if (aligned_needed > sizeof(r->out_buf)) {
            ESP_LOGE(TAG, "pre-prepare: request too large to embed, dropping (%zu > %zu)",
                     aligned_needed, sizeof(r->out_buf));
            rqueue_pop(src_queue);
            continue;
        }

        /* MEDIUM FIX M1: Clear out_buf before message construction to prevent
         * stale data from leaking into authenticator computation or padding. */
        memset(r->out_buf, 0, sizeof(r->out_buf));

        tbft_pre_prepare_rep_t *pp = (tbft_pre_prepare_rep_t *)r->out_buf;
        pp->hdr.tag          = TBFT_MSG_PRE_PREPARE;
        pp->hdr.extra        = 0;
        pp->hdr.timestamp_us = esp_timer_get_time();
        pp->view             = r->node.view;
        pp->seqno            = r->seqno;
        pp->rset_size        = req->len;
        pp->non_det_size     = (int16_t)ndet_len;

        /* Append request set and non-det choices; the digest covers both, so
         * nobody can alter the non-deterministic choices in transit. */
        uint8_t *ptr = r->out_buf + sizeof(*pp);
        memcpy(ptr, req->buf, (size_t)req->len);
        ptr += req->len;
        memcpy(ptr, r->ndet_buf, (size_t)ndet_len);
        ptr += ndet_len;
        tbft_msg_digest(r->out_buf + sizeof(*pp), (size_t)req->len + (size_t)ndet_len,
                        &pp->digest);

        /* Authenticator.  The header's size field is covered by the MAC:
         * set it before computing the MAC. */
        tbft_auth_t *auth = (tbft_auth_t *)ptr;
        int32_t msg_len_before_auth = (int32_t)(ptr - r->out_buf);
        pp->hdr.size = tbft_msg_align(msg_len_before_auth + (int32_t)sizeof(tbft_auth_t));
        tbft_node_gen_auth(&r->node, r->out_buf, (size_t)msg_len_before_auth, auth);

        if (!tbft_ar_store_pp(&r->ar, r->seqno, r->out_buf, pp->hdr.size)) {
            ESP_LOGE(TAG, "pre-prepare: cannot store seqno=%lld", (long long)r->seqno);
            break;
        }
        tbft_node_send(&r->node, r->out_buf, (size_t)pp->hdr.size, TBFT_ALL_REPLICAS);
        ESP_LOGI(TAG, "sent pre-prepare seqno=%lld view=%lld (rset=%d, ndet=%d, total=%d)",
                 (long long)r->seqno, (long long)r->node.view,
                 req->len, ndet_len, pp->hdr.size);

        /* The primary sends no Prepare: its pre-prepare stands for it. */
        r->last_assigned_rid[rr->cid] = rr->rid;
        rqueue_pop(src_queue);
        r->seqno++;
    }

    if (!tbft_replica_has_pending_requests(r)) {
        /* No pending requests — stop the view-change timer so it doesn't
         * fire during idle periods. */
        tbft_itimer_stop(&r->vtimer);
    } else if (!tbft_itimer_is_running(&r->vtimer)) {
        tbft_itimer_start(&r->vtimer, vtimer_period(r));
    }
}



void tbft_replica_execute_committed(tbft_replica_t *r)
{
    /* PBFT executes strictly in sequence-number order.  A seqno without its
     * commit quorum or pre-prepare stops execution until retransmission
     * (Status) or a state fetch fills the gap: skipping it would make this
     * replica's state, and the replies it sends, diverge. */
    if (tbft_state_in_fetch(&r->state)) return;   /* the state is being replaced */

    for (;;) {
        tbft_seqno_t n = r->last_executed + 1;
        if (!tbft_replica_in_window(r, n) || !tbft_ar_in_range(&r->ar, n)) break;
        if (!tbft_ar_committed(&r->ar, n)) {
            /* a later seqno committed already: this one's messages got lost */
            for (tbft_seqno_t m = n + 1; tbft_ar_in_range(&r->ar, m); m++) {
                if (tbft_ar_committed(&r->ar, m)) {
                    ask_for_missing(r);
                    break;
                }
            }
            break;
        }

        int pp_len = 0;
        const uint8_t *pp_buf = tbft_ar_load_pp(&r->ar, n, &pp_len);
        if (!pp_buf) {
            ESP_LOGI(TAG, "execute: seqno=%lld committed, waiting for its pre-prepare",
                     (long long)n);
            ask_for_missing(r);
            break;
        }
        const tbft_pre_prepare_rep_t *pp = (const tbft_pre_prepare_rep_t *)pp_buf;
        tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, n);
        const tbft_commit_rep_t *win =
            (const tbft_commit_rep_t *)tbft_commit_cert_cvalue(&sl->commit_cert);
        if (!win || !tbft_digest_equal(&pp->digest, &win->digest)) {
            ESP_LOGW(TAG, "execute: seqno=%lld pre-prepare differs from the commit quorum"
                     " - waiting for the right one", (long long)n);
            ask_for_missing(r);
            break;
        }

        execute_one(r, n, pp);
        r->last_executed = n;
        if (r->last_prepared < n) r->last_prepared = n;
        r->stat_executed++;
        r->vc_backoff_shift = 0;   /* progress */

        if ((n % TBFT_CHECKPOINT_INTERVAL) == 0) {
            take_checkpoint(r, n);
        }
    }
    update_vtimer(r);
}

void tbft_replica_mark_stable(tbft_replica_t *r, tbft_seqno_t seqno)
{
    if (seqno <= r->last_stable) return;
    const tbft_digest_t *winning = tbft_cr_winning_digest(&r->cr, seqno);
    if (!winning) return;
    tbft_digest_t target = *winning;

    /* The quorum discards its messages up to a stable checkpoint, so a
     * replica that has not executed that far must fetch the state.  Execute
     * first: often the missing seqnos are committed here already. */
    if (r->last_executed < seqno) {
        tbft_replica_execute_committed(r);
        if (seqno <= r->last_stable) return;   /* became stable meanwhile */
    }

    tbft_digest_t mine;
    if (r->last_executed >= seqno &&
        tbft_state_get_checkpoint_digest(&r->state, seqno, &mine)) {
        if (tbft_digest_equal(&target, &mine)) {
            install_stable(r, seqno);
            /* the window moved: propose what waits in the queue */
            if (tbft_replica_is_primary(r)) tbft_replica_send_pre_prepare(r);
            return;
        }
        if (start_fetch(r, seqno, &target)) {
            ESP_LOGE(TAG, "checkpoint seqno=%lld: local state differs from the quorum"
                     " - fetching", (long long)seqno);
        }
        return;
    }

    /* Behind.  The others keep the messages of the last checkpoint period
     * for a while: ask for them first, and fetch the state only if that has
     * not brought us there half a Status period later. */
    int64_t now = esp_timer_get_time();
    if (r->stable_wait_seqno == 0) r->stable_wait_us = now;   /* just fell behind */
    r->stable_wait_seqno = seqno;
    if (!tbft_state_in_fetch(&r->state) && now - r->stable_wait_us < r->stimer_period_us / 2) {
        ask_for_missing(r);
        return;
    }
    if (start_fetch(r, seqno, &target)) {
        ESP_LOGW(TAG, "checkpoint seqno=%lld is stable at the quorum, executed here up to"
                 " %lld - fetching", (long long)seqno, (long long)r->last_executed);
    }
}

void tbft_replica_send_view_change(tbft_replica_t *r)
{
    /* Timer expiry: ask for the next view, or for the one after a view
     * change that did not complete in time. */
    tbft_view_t target = r->vi.in_progress ? r->vi.target_view + 1 : r->node.view + 1;
    send_view_change_to(r, target);
}

static void send_view_change_to(tbft_replica_t *r, tbft_view_t target)
{
    tbft_vi_set_target(&r->vi, target);
    r->vi.in_progress = true;

    memset(r->out_buf, 0, sizeof(r->out_buf));
    uint8_t *ptr = r->out_buf;
    tbft_view_change_rep_t *vc = (tbft_view_change_rep_t *)ptr;
    vc->hdr.tag          = TBFT_MSG_VIEW_CHANGE;
    vc->hdr.extra        = 0;
    vc->hdr.timestamp_us = esp_timer_get_time();
    vc->v                = target;
    vc->ls               = r->last_stable;
    vc->id               = r->node.node_id;
    vc->_pad             = 0;
    vc->n_ckpts          = 0;
    vc->n_reqs           = 0;
    ptr += sizeof(*vc);

    /* Our stable checkpoint with its state digest (while it is still being
     * fetched, the digest the fetch aims at) */
    tbft_digest_t ls_digest;
    bool have_ls = false;
    if (r->last_stable > 0) {
        if (tbft_state_get_checkpoint_digest(&r->state, r->last_stable, &ls_digest)) {
            have_ls = true;
        } else if (tbft_state_in_fetch(&r->state) && r->state.fetch_has_digest &&
                   r->state.fetch_seqno == r->last_stable) {
            ls_digest = r->state.fetch_digest;
            have_ls   = true;
        }
    }
    if (have_ls) {
        tbft_vc_ckpt_t *ckpt = (tbft_vc_ckpt_t *)ptr;
        ckpt->seqno  = r->last_stable;
        ckpt->digest = ls_digest;
        ptr += sizeof(*ckpt);
        vc->n_ckpts = 1;
    }

    /* Every seqno above it that we saw prepared or committed, with the
     * highest view that happened in: the new primary re-proposes these. */
    for (tbft_seqno_t s = r->last_stable + 1; s <= r->last_stable + TBFT_WINDOW_SIZE; s++) {
        if (!tbft_ar_in_range(&r->ar, s)) continue;
        const tbft_agreement_slice_t *sl = tbft_ar_slice(&r->ar, s);
        if (sl->prep_view < 0) continue;
        if ((size_t)(ptr - r->out_buf) + sizeof(tbft_vc_req_info_t) + sizeof(tbft_sig_t)
                > sizeof(r->out_buf)) {
            break;
        }
        tbft_vc_req_info_t *req = (tbft_vc_req_info_t *)ptr;
        req->seqno     = s;
        req->last_view = sl->prep_view;
        req->digest    = sl->prep_digest;
        ptr += sizeof(*req);
        vc->n_reqs++;
    }

    vc->hdr.size = tbft_msg_align((int32_t)(ptr - r->out_buf));
    int32_t body_size  = vc->hdr.size;
    int32_t total_size = body_size + (int32_t)sizeof(tbft_sig_t);
    if (total_size > (int32_t)sizeof(r->out_buf)) {
        ESP_LOGE(TAG, "send_view_change: out_buf too small for signature");
        return;
    }
    /* View-change messages MUST be signed: abort rather than send unsigned. */
    tbft_sig_t *sig = (tbft_sig_t *)(r->out_buf + body_size);
    if (tbft_node_gen_sig(&r->node, r->out_buf, (size_t)body_size, sig) != 0) {
        ESP_LOGE(TAG, "send_view_change: failed to sign - refusing to send unsigned");
        return;
    }
    tbft_node_send(&r->node, r->out_buf, (size_t)total_size, TBFT_ALL_REPLICAS);
    r->stat_view_change_sent++;
    ESP_LOGW(TAG, "sent view-change to view %lld (ls=%lld, %d prepared)",
             (long long)target, (long long)r->last_stable, (int)vc->n_reqs);

    (void)tbft_vi_collect_vc(&r->vi, r->node.node_id, r->out_buf, total_size);
    if (target > r->vc_view_seen[r->node.node_id]) {
        r->vc_view_seen[r->node.node_id] = target;
    }

    /* Wait longer for each view change in a row without progress. */
    if (r->vc_backoff_shift < 6) r->vc_backoff_shift++;
    tbft_itimer_start(&r->vtimer, vtimer_period(r));

    maybe_send_new_view(r);
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
    nk->hdr.tag          = TBFT_MSG_NEW_KEY;
    nk->hdr.extra        = 0;
    nk->hdr.timestamp_us = esp_timer_get_time();
    nk->id               = local_id;
    nk->n_keys           = 0;
    /* The keys we hold: a replica whose key is missing here answers with
     * its New_key (e.g. after we rebooted or booted late). */
    nk->have_keys        = 0;
    for (int i = 0; i < n; i++) {
        if (i != local_id && r->node.principals[i] && r->node.principals[i]->keys_fresh) {
            nk->have_keys |= 1ULL << i;
        }
    }

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

    /* The sender lacks our key (it rebooted or booted late): answer with our
     * New_key, at most every 2 s (one broadcast serves every such replica). */
    if (!(nk->have_keys & (1ULL << r->node.node_id))) {
        int64_t now = esp_timer_get_time();
        if (r->last_nk_reply_us == 0 || now - r->last_nk_reply_us >= 2000000LL) {
            r->last_nk_reply_us = now;
            ESP_LOGI(TAG, "replica %d lacks our key: re-sending New_key", sender_id);
            tbft_replica_send_new_key(r);
        }
    }

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
    tbft_principal_set_in_key(p, &new_key);
    
    /* Save the ciphertext to skip duplicate processing in the future */
    memcpy(p->last_new_key_ciphertext, slots[our_slot_idx].ciphertext, TBFT_SIG_SIZE);

    ESP_LOGI(TAG, "installed HMAC in-key from replica %d", sender_id);

    /* Explicit wipe of plaintext key after installation */
    memset(&new_key, 0, sizeof(new_key));
}
