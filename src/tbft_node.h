#pragma once

#include "tbft_types.h"
#include "tbft_message.h"
#include "tbft_principal.h"
#include "tbft_transport.h"
#include "tbft_itimer.h"
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Node — base for all PBFT participants (both replicas and clients).
 *
 * Owns:
 *  - The local node's crypto identity (node_id, private key)
 *  - Array of all known principals (replicas first, then clients)
 *  - Transport layer (UDP or ESP-NOW)
 *  - Authentication freshness timer
 * -------------------------------------------------------------------------- */

typedef struct tbft_node tbft_node_t;

struct tbft_node {
    /* Protocol identity */
    tbft_node_id_t  node_id;
    int             max_faulty;    /* f: max Byzantine faults */
    int             num_replicas;  /* n = 3f + 1 */
    int             threshold;     /* 2f + 1 */
    tbft_view_t     view;
    int             cur_primary;   /* view % num_replicas */

    /* Principal registry */
    tbft_principal_t  *principals[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    int                num_principals;

    /* Local principal (shortcut into principals[node_id]) */
    tbft_principal_t  *local_principal;

    /* Transport layer (UDP or ESP-NOW) */
    tbft_transport_t   *transport;

    /* Receive buffer */
    uint8_t   recv_buf[TBFT_MAX_MESSAGE_SIZE];

    /* Authentication freshness timer */
    tbft_itimer_t  atimer;
    int64_t        auth_timeout_us;

    /* Monotonic request-id counter */
    uint64_t       rid_counter;
};

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Initialise the node structure from a parsed configuration.
 *
 * @param node           Node to initialise
 * @param node_id        This node's id
 * @param f              Max Byzantine faults
 * @param num_nodes      Total node count (replicas + clients)
 * @param mcast_ip       Multicast group IP string (UDP only, e.g. "234.5.6.8")
 * @param auth_timeout_us  Authentication timer period in microseconds
 * @param port           UDP port to bind (UDP only; ignored for ESP-NOW)
 * @return 0 on success, -1 on error
 */
int tbft_node_init(tbft_node_t *node, tbft_node_id_t node_id,
                   int f, int num_nodes,
                   const char *mcast_ip, int64_t auth_timeout_us,
                   uint16_t port);

/**
 * Release all resources held by the node.
 */
void tbft_node_free(tbft_node_t *node);

/* --------------------------------------------------------------------------
 * Network I/O (delegated to transport layer)
 * -------------------------------------------------------------------------- */

/**
 * Send a message to a destination.
 * @param node  This node
 * @param buf   Message bytes
 * @param len   Message length
 * @param dest  Destination node id, or TBFT_ALL_REPLICAS (-1) for broadcast
 * @return bytes sent, or -1 on error
 */
int tbft_node_send(tbft_node_t *node, const void *buf, size_t len,
                   tbft_node_id_t dest);

/**
 * Receive next message (non-blocking).
 * @param node     This node
 * @param buf      Output buffer (must be TBFT_MAX_MESSAGE_SIZE bytes)
 * @param src_id   If non-NULL, receives the apparent sender id (best-effort)
 * @return number of bytes received, 0 if nothing available, -1 on error
 */
int tbft_node_recv(tbft_node_t *node, void *buf, tbft_node_id_t *src_id);

/* --------------------------------------------------------------------------
 * Authenticator generation / verification
 * -------------------------------------------------------------------------- */

/**
 * Fill an outgoing authenticator: compute HMAC for each remote replica
 * using the respective out-key, and store into auth->slots[].
 *
 * Slot layout: auth->slots[i] holds the HMAC for replica whose index in
 * the replica list != node_id.  Slot ordering follows replica indices
 * with the local node's slot skipped.
 *
 * @param node    This node
 * @param msg     Message bytes to authenticate (header + body)
 * @param msg_len Message length
 * @param auth    Output authenticator
 */
void tbft_node_gen_auth(tbft_node_t *node, const void *msg, size_t msg_len,
                        tbft_auth_t *auth);

/**
 * Verify one slot of a received authenticator from principal @p sender_id.
 *
 * @param node       This node
 * @param sender_id  Principal that sent the message
 * @param msg        Message bytes (header + body, excluding authenticator)
 * @param msg_len    Length
 * @param mac        The MAC slot from the received authenticator
 * @return true if valid
 */
bool tbft_node_verify_auth(tbft_node_t *node, tbft_node_id_t sender_id,
                           const void *msg, size_t msg_len,
                           const tbft_mac_t *mac);

/* --------------------------------------------------------------------------
 * RSA signature path
 * -------------------------------------------------------------------------- */

/**
 * Sign data with the local node's private key.
 */
int tbft_node_gen_sig(tbft_node_t *node, const void *msg, size_t msg_len,
                      tbft_sig_t *sig);

/**
 * Verify a signature with principal @p sender_id's public key.
 */
bool tbft_node_verify_sig(tbft_node_t *node, tbft_node_id_t sender_id,
                          const void *msg, size_t msg_len,
                          const tbft_sig_t *sig);

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/**
 * Return the primary replica for the given view.
 */
static inline int tbft_node_primary(const tbft_node_t *node, tbft_view_t v)
{
    return (int)(v % (tbft_view_t)node->num_replicas);
}

/**
 * Return true if @p node_id is a replica (index < num_replicas).
 */
static inline bool tbft_node_is_replica(const tbft_node_t *node,
                                        tbft_node_id_t id)
{
    return id >= 0 && id < node->num_replicas;
}

/**
 * Allocate and return a fresh request ID for this node.
 */
tbft_req_id_t tbft_node_new_rid(tbft_node_t *node);
