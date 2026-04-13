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
 * @param mcast_ip    Multicast IP string (UDP only; NULL for ESP-NOW)
 * @param port        UDP bind port (UDP only; ignored for ESP-NOW)
 * @return 0 on success, -1 on error
 */
int tbft_transport_create(tbft_transport_t **out,
                          tbft_transport_type_t type,
                          int num_nodes,
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
