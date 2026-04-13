#pragma once

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
 *  - Sequence number tracking
 *  - Request queues
 *  - Protocol logs (plog, clog, elog)
 *  - Static memory regions (agreement, checkpoint, special)
 *  - State management
 *  - View-change support
 *  - Timers
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
    tbft_seqno_t  last_prepared;          /* highest prepared seqno */
    tbft_seqno_t  last_executed;          /* highest committed + executed */
    tbft_seqno_t  last_tentative_execute; /* highest tentatively executed */

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
    tbft_itimer_t rtimer;  /* recovery */
    tbft_itimer_t ntimer;  /* null-request (keep-alive) */

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
    bool  running;
} tbft_replica_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise the replica.
 * @param r               Replica to initialise
 * @param node_id         This replica's id
 * @param f               Max Byzantine faults
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
                      tbft_node_id_t node_id, int f, int num_nodes,
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

/** Broadcast Prepare for sequence number @p n */
void tbft_replica_send_prepare(tbft_replica_t *r, tbft_seqno_t n);

/** Broadcast Commit for sequence number @p n */
void tbft_replica_send_commit(tbft_replica_t *r, tbft_seqno_t n);

/** Execute all committed-but-unexecuted requests in order */
void tbft_replica_execute_committed(tbft_replica_t *r);

/** Mark @p seqno as stable: truncate logs and GC */
void tbft_replica_mark_stable(tbft_replica_t *r, tbft_seqno_t seqno);

/** Initiate a view change to view v+1 */
void tbft_replica_send_view_change(tbft_replica_t *r);

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
