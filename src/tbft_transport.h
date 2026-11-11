#pragma once

#include "tbft_types.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Transport abstraction layer
 *
 * Supports two backends:
 *   TBFT_TRANSPORT_UDP      — UDP over lwIP sockets (default)
 *   TBFT_TRANSPORT_ESPNOW   — ESP-NOW with automatic fragmentation/reassembly
 *
 * Switching is controlled by CONFIG_TBFT_TRANSPORT_TYPE in Kconfig.
 * The public API (tbft_node_send/recv) is identical regardless of backend.
 * -------------------------------------------------------------------------- */

typedef enum {
    TBFT_TRANSPORT_UDP = 0,
    TBFT_TRANSPORT_ESPNOW = 1,
} tbft_transport_type_t;

/** Opaque transport handle */
typedef struct tbft_transport tbft_transport_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * Create and initialise a transport instance.
 *
 * @param out         Output pointer
 * @param type        UDP or ESP-NOW
 * @param num_nodes   Total node count (for peer table sizing)
 * @param num_replicas  Replica count (3f+1); TBFT_ALL_REPLICAS broadcast
 *                      targets only nodes 0..num_replicas-1
 * @param mcast_ip    Multicast IP string (UDP only; NULL for ESP-NOW)
 * @param port        UDP bind port (UDP only; ignored for ESP-NOW)
 * @return 0 on success, -1 on error
 */
int tbft_transport_create(tbft_transport_t **out,
                           tbft_transport_type_t type,
                           int num_nodes,
                           int num_replicas,
                           const char *mcast_ip,
                           uint16_t port);

/** Release all resources held by the transport. */
void tbft_transport_free(tbft_transport_t *t);

/* --------------------------------------------------------------------------
 * Peer registration
 * -------------------------------------------------------------------------- */

/**
 * Register the address of peer @p node_id.
 *
 * For UDP:   addr->u.udp.ip/port must be set.
 * For ESP-NOW: addr->u.mac.bytes must contain the peer's MAC address.
 */
void tbft_transport_set_peer(tbft_transport_t *t,
                             tbft_node_id_t node_id,
                             const tbft_addr_t *addr);

/* --------------------------------------------------------------------------
 * Send / Recv
 * -------------------------------------------------------------------------- */

/**
 * Send a message to a destination.
 *
 * @param dest  Destination node id, or TBFT_ALL_REPLICAS for broadcast
 * @return bytes sent, or -1 on error
 *
 * ESP-NOW backend automatically fragments messages exceeding the limit.
 */
int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len,
                        tbft_node_id_t dest);

/**
 * Receive the next complete message (non-blocking).
 *
 * @param buf      Output buffer (must be TBFT_MAX_MESSAGE_SIZE bytes)
 * @param buf_len  Size of @p buf
 * @param src_id   If non-NULL, receives the apparent sender id
 * @return bytes received, 0 if nothing available, -1 on error
 *
 * ESP-NOW backend automatically reassembles fragmented packets.
 */
int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len,
                        tbft_node_id_t *src_id);

/* --------------------------------------------------------------------------
 * Statistics (A8) - plain counters, cheap enough to keep always on
 * -------------------------------------------------------------------------- */
#define TBFT_STATS_NUM_TAGS 18   /* tbft_msg_tag_t values are 1..17 */

typedef struct {
    uint32_t tx_msgs;        /* logical messages accepted for sending */
    uint32_t tx_fail;        /* sends given up on (retries exhausted / hard error) */
    uint32_t tx_queue_full;  /* ESP-NOW: send queue full, message dropped */
    uint32_t rx_msgs;        /* complete messages handed to the protocol */
    uint32_t rx_drop;        /* ESP-NOW: complete message dropped (rx queue full) */
    uint32_t rx_frag_drop;   /* ESP-NOW: fragment dropped (bad header, reasm) */
    uint32_t tx_by_tag[TBFT_STATS_NUM_TAGS];
    uint32_t rx_by_tag[TBFT_STATS_NUM_TAGS];
} tbft_transport_stats_t;

/** Copy current counters (not atomic across fields; fine for reporting). */
void tbft_transport_get_stats(tbft_transport_t *t, tbft_transport_stats_t *out);
