#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "tbft_node.h"
#include "tbft_state.h"
#include "tbft_agreement_region.h"
#include "tbft_checkpoint_region.h"
#include "tbft_special_region.h"
#include "tbft_view_info.h"
#include "tbft_prepared_cert.h"

/* --------------------------------------------------------------------------
 * Replica — the PBFT state machine (section 5 / Replica).
 *
 * Extends tbft_node_t with:
 *  - Sequence number tracking (seqno, last_stable, last_executed, ...)
 *  - Request queues (rqueue, ro_rqueue)
 *  - Static memory regions: ar (agreement), cr (checkpoint), sr (special)
 *  - View-change protocol (vi)
 *  - Timers (vtimer, stimer, rtimer, ntimer)
 *  - Application callbacks and buffers
 * -------------------------------------------------------------------------- */

/* TBFT_RQUEUE_MAX sourced from Kconfig (default 16) */

/* Simple request queue entry */
typedef struct {
    uint8_t  buf[TBFT_MAX_MESSAGE_SIZE];
    int      len;
    bool     ro; /* read-only */
    bool     used;
} tbft_rqueue_entry_t;

/** Request queue (circular FIFO) */
typedef struct {
    tbft_rqueue_entry_t entries[TBFT_RQUEUE_MAX];
    int head;
    int tail;
    int count;
} tbft_rqueue_t;

/* Client table entry: the last request executed for one client and its
 * reply.  The table lives in the library part of the replicated state
 * (tbft_state blocks after the application's), so checkpoints cover it and a
 * state fetch brings it along: every replica agrees which requests already
 * ran, never executes one twice, and can answer a retransmitted request. */
typedef struct {
    tbft_req_id_t last_rid;    /* last executed request id, 0 = none */
    tbft_seqno_t  seqno;       /* sequence number it executed at */
    int32_t       reply_len;   /* bytes in reply[]; -1 = rejected, no reply */
    int32_t       _pad;
    uint8_t       reply[TBFT_MAX_REPLY_SIZE];
} tbft_client_entry_t;

static constexpr size_t TBFT_SYS_STATE_SIZE =
    (TBFT_MAX_NUM_CLIENTS * sizeof(tbft_client_entry_t) + TBFT_BLOCK_SIZE - 1)
    / TBFT_BLOCK_SIZE * TBFT_BLOCK_SIZE;

/* Execution callback: called when a request is committed and ready to execute */
typedef int (*tbft_exec_cb_t)(const void *req, int req_len,
                              void *rep, int *rep_len,
                              void *ndet, int ndet_len,
                              int client_id, bool read_only);

/* Non-deterministic choices callback */
typedef void (*tbft_comp_ndet_cb_t)(tbft_seqno_t seqno,
                                    void *ndet, int *ndet_len,
                                    int max_len);

/* Reply callback: called when a reply is ready to send to the client */
typedef void (*tbft_recv_reply_cb_t)(const void *rep, int rep_len,
                                     int client_id);

typedef struct {
    tbft_node_t  node;   /* MUST be first — pointer aliasing with node */

    /* Sequence number state */
    tbft_seqno_t  seqno;                  /* next seqno to assign (primary) */
    tbft_seqno_t  last_stable;            /* last stable checkpoint seqno */
    int64_t       last_fetch_throttle_us;   /* last time we initiated a state fetch */
    tbft_seqno_t  last_prepared;          /* highest prepared seqno */
    tbft_seqno_t  last_executed;          /* highest committed + executed */
    tbft_seqno_t  last_tentative_execute; /* highest tentatively executed */

    /* Client request tracking for idle timer suppression */
    tbft_req_id_t last_received_rid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    tbft_req_id_t last_executed_rid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    tbft_req_id_t last_forwarded_rid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    /* Primary: highest request id of each client given a sequence number */
    tbft_req_id_t last_assigned_rid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];

    /* Library part of the replicated state: tbft_client_entry_t per client */
    alignas(8) uint8_t sys_state[TBFT_SYS_STATE_SIZE];

    /* View change: highest view each replica asked for in a View_change.
     * A replica joins a view change only when f+1 others ask for one. */
    tbft_view_t   vc_view_seen[TBFT_MAX_NUM_REPLICAS];
    int           vc_backoff_shift;       /* consecutive view changes without progress */

    /* Latest Status of each replica (drives retransmission) */
    tbft_view_t   peer_view[TBFT_MAX_NUM_REPLICAS];
    bool          peer_in_vc[TBFT_MAX_NUM_REPLICAS];
    int64_t       peer_status_us[TBFT_MAX_NUM_REPLICAS];
    /* Our own progress at the time of each replica's previous Status */
    tbft_seqno_t  peer_seen_top[TBFT_MAX_NUM_REPLICAS];
    tbft_seqno_t  peer_seen_stable[TBFT_MAX_NUM_REPLICAS];
    int64_t       retx_us[TBFT_MAX_NUM_REPLICAS];   /* last retransmission to each */
    int64_t       last_ask_us;                      /* last Status asking for a missing message */
    /* A checkpoint the quorum certified before we executed that far: when
     * we learned of it (we ask for the missing messages first and fetch the
     * state only if that does not get us there) */
    tbft_seqno_t  stable_wait_seqno;
    int64_t       stable_wait_us;
    int64_t       last_nk_reply_us;                 /* last New_key sent on request */
    int64_t       reply_sent_us[TBFT_MAX_NUM_CLIENTS]; /* last reply sent to each client */

    /* Request queues */
    tbft_rqueue_t rqueue;    /* read-write requests */
    tbft_rqueue_t ro_rqueue; /* read-only requests */

    /* Static memory regions (TinyBFT) */
    tbft_agreement_region_t   ar;
    tbft_checkpoint_region_t  cr;
    tbft_special_region_t     sr;

    /* Application state */
    tbft_state_t  state;

    /* View-change protocol */
    tbft_view_info_t vi;

    /* Timers */
    tbft_itimer_t vtimer;  /* view-change timeout */
    tbft_itimer_t stimer;  /* status broadcast */
    /* rtimer (recovery) and ntimer (keep-alive) reserved for future use */

    /* Timer periods */
    int64_t vtimer_period_us;
    int64_t stimer_period_us;

    /* Application callbacks */
    tbft_exec_cb_t        exec_cb;
    tbft_comp_ndet_cb_t   comp_ndet_cb;
    tbft_recv_reply_cb_t  recv_reply_cb;
    int                   ndet_max_len;

    /* Non-deterministic choices buffer */
    uint8_t  ndet_buf[TBFT_NDET_BUF_SIZE];

    /* Outgoing message build buffer */
    uint8_t  out_buf[TBFT_MAX_MESSAGE_SIZE];

    /* Running flag */
    volatile bool  running;

    /* Yield counter for periodic cooperative scheduling.
     * Incremented each message loop iteration; when it reaches the
     * threshold the replica yields to let lower-priority tasks run.
     * Reset to 0 on replica restart to avoid stale values. */
    int yield_counter;

    /* Protocol event counters (reported with transport stats) */
    uint32_t stat_executed;
    uint32_t stat_commit_resent;
    uint32_t stat_view_change_sent;
    uint32_t stat_new_view;
    uint32_t stat_fetch_started;

    /* Event group for decoupling timer callbacks from heavy processing */
    EventGroupHandle_t evt_group;
    StaticEventGroup_t evt_group_buf;   /* its storage (no heap) */
} tbft_replica_t;

static constexpr EventBits_t TBFT_EVT_VTIMER = 1 << 0;
static constexpr EventBits_t TBFT_EVT_STIMER = 1 << 1;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise the replica.
 * @param r               Replica to initialise
 * @param node_id         This replica's id
 * @param f               Max Byzantine faults
 * @param num_replicas    Actual replica count; may be less than 3f+1 for demos
 * @param num_nodes       Total nodes (replicas + clients)
 * @param mcast_ip        Multicast IP string
 * @param auth_timeout_us Authentication freshness timeout
 * @param port            UDP port
 * @param state_mem       Application state buffer
 * @param state_size      State buffer size
 * @param exec_cb         Execution callback
 * @param comp_ndet_cb    Non-deterministic choices callback
 * @param ndet_max_len    Max bytes for non-deterministic choices
 * @param recv_reply_cb   Reply notification callback
 * @return 0 on success, -1 on failure
 */
int tbft_replica_init(tbft_replica_t *r,
                      tbft_node_id_t node_id, int f, int num_replicas, int num_nodes,
                      const char *mcast_ip, int64_t auth_timeout_us,
                      uint16_t port,
                      void *state_mem, size_t state_size,
                      tbft_exec_cb_t exec_cb,
                      tbft_comp_ndet_cb_t comp_ndet_cb,
                      int ndet_max_len,
                      tbft_recv_reply_cb_t recv_reply_cb);

/**
 * Release all resources held by the replica.
 */
void tbft_replica_free(tbft_replica_t *r);

/* --------------------------------------------------------------------------
 * Main event loop
 * -------------------------------------------------------------------------- */

/**
 * Run the replica event loop (blocking — call from a dedicated FreeRTOS task).
 */
void tbft_replica_run(tbft_replica_t *r);

/** Print one "TBFT_STATS key=value ..." line (WARN level, parseable). */
void tbft_replica_print_stats(const tbft_replica_t *r);

/* --------------------------------------------------------------------------
 * Message handlers (called from tbft_replica_run dispatch)
 * -------------------------------------------------------------------------- */

void tbft_replica_handle_request(tbft_replica_t *r,
                                 const void *msg, int len);
void tbft_replica_handle_pre_prepare(tbft_replica_t *r,
                                     const void *msg, int len);
void tbft_replica_handle_prepare(tbft_replica_t *r,
                                 const void *msg, int len);
void tbft_replica_handle_commit(tbft_replica_t *r,
                                const void *msg, int len);
void tbft_replica_handle_checkpoint(tbft_replica_t *r,
                                    const void *msg, int len);
void tbft_replica_handle_view_change(tbft_replica_t *r,
                                     const void *msg, int len);
void tbft_replica_handle_new_view(tbft_replica_t *r,
                                  const void *msg, int len);
void tbft_replica_handle_fetch(tbft_replica_t *r,
                               const void *msg, int len);
void tbft_replica_handle_meta_data(tbft_replica_t *r,
                                   const void *msg, int len);
void tbft_replica_handle_data(tbft_replica_t *r,
                              const void *msg, int len);
void tbft_replica_handle_new_key(tbft_replica_t *r,
                                 const void *msg, int len);

/* --------------------------------------------------------------------------
 * Protocol actions
 * -------------------------------------------------------------------------- */

/** Generate fresh HMAC session keys and broadcast a New_key message */
void tbft_replica_send_new_key(tbft_replica_t *r);

/** Primary: assign seqno and broadcast Pre_prepare for queued requests */
void tbft_replica_send_pre_prepare(tbft_replica_t *r);

/** Execute all committed-but-unexecuted requests in order */
void tbft_replica_execute_committed(tbft_replica_t *r);

/** Mark @p seqno as stable: truncate logs and GC */
void tbft_replica_mark_stable(tbft_replica_t *r, tbft_seqno_t seqno);

/** Initiate a view change to view v+1 */
void tbft_replica_send_view_change(tbft_replica_t *r);

/** True if there is at least one verified client request pending execution */
bool tbft_replica_has_pending_requests(const tbft_replica_t *r);

/** True if @p seqno is within the current window */
static inline bool tbft_replica_in_window(const tbft_replica_t *r,
                                          tbft_seqno_t seqno)
{
    return seqno > r->last_stable &&
           seqno <= r->last_stable + TBFT_WINDOW_SIZE;
}

/** True if this replica is the current primary */
static inline bool tbft_replica_is_primary(const tbft_replica_t *r)
{
    return r->node.node_id == tbft_node_primary(&r->node, r->node.view);
}
