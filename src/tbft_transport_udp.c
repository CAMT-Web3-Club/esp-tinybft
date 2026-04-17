/**
 * @file tbft_transport_udp.c
 * @brief UDP transport backend — lwIP sockets.
 *
 * Extracted from the original tbft_node.c socket logic.
 */

#include "tbft_transport.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "tbft_udp";

/* --------------------------------------------------------------------------
 * Internal struct
 * -------------------------------------------------------------------------- */

typedef struct {
    int sock;
    struct sockaddr_in mcast_addr;
    bool use_multicast;
    /* Peer address table — indexed by node_id */
    tbft_addr_t  peers[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    bool         peer_valid[TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS];
    int          num_nodes;
    int          num_replicas;
} tbft_udp_t;

/* --------------------------------------------------------------------------
 * Socket setup
 * -------------------------------------------------------------------------- */

static int socket_open(uint16_t port, bool use_multicast,
                       const char *mcast_ip, struct sockaddr_in *mcast_addr_out)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        return -1;
    }

    /* Enable address reuse */
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    /* Set non-blocking */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    /* Bind to port */
    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(port),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind(%d) failed: %d", port, errno);
        close(sock);
        return -1;
    }

    if (use_multicast && mcast_ip) {
        /* Join multicast group */
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed: %d (continuing)", errno);
        }

        /* Set multicast TTL */
        uint8_t ttl = 32;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        /* Store multicast destination address */
        memset(mcast_addr_out, 0, sizeof(*mcast_addr_out));
        mcast_addr_out->sin_family      = AF_INET;
        mcast_addr_out->sin_addr.s_addr = inet_addr(mcast_ip);
        mcast_addr_out->sin_port        = htons(port);
    }

    return sock;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int tbft_transport_create(tbft_transport_t **out,
                           tbft_transport_type_t type,
                           int num_nodes,
                           int num_replicas,
                           const char *mcast_ip,
                           uint16_t port)
{
    (void)type;  /* only called with TBFT_TRANSPORT_UDP */

    if (num_nodes > TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS) {
        ESP_LOGE(TAG, "num_nodes %d exceeds max %d", num_nodes,
                 TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS);
        return -1;
    }

    tbft_udp_t *udp = (tbft_udp_t *)calloc(1, sizeof(*udp));
    if (!udp) return -1;

    udp->num_nodes    = num_nodes;
    udp->num_replicas = num_replicas;

#ifdef CONFIG_TBFT_DISABLE_MULTICAST
    udp->use_multicast = false;
#else
    udp->use_multicast = (mcast_ip != NULL);
#endif

    udp->sock = socket_open(port, udp->use_multicast, mcast_ip,
                            &udp->mcast_addr);
    if (udp->sock < 0) {
        free(udp);
        return -1;
    }

    *out = (tbft_transport_t *)udp;
    ESP_LOGI(TAG, "UDP transport init: port=%d mcast=%s", port,
             udp->use_multicast ? (mcast_ip ? mcast_ip : "yes") : "no");
    return 0;
}

void tbft_transport_free(tbft_transport_t *t)
{
    if (!t) return;
    tbft_udp_t *udp = (tbft_udp_t *)t;
    if (udp->sock >= 0) {
        close(udp->sock);
    }
    free(udp);
}

void tbft_transport_set_peer(tbft_transport_t *t,
                             tbft_node_id_t node_id,
                             const tbft_addr_t *addr)
{
    if (!t || node_id < 0 || node_id >= TBFT_MAX_NUM_REPLICAS + TBFT_MAX_NUM_CLIENTS)
        return;
    tbft_udp_t *udp = (tbft_udp_t *)t;
    udp->peers[node_id] = *addr;
    udp->peer_valid[node_id] = true;
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len,
                        tbft_node_id_t dest)
{
    tbft_udp_t *udp = (tbft_udp_t *)t;
    if (!udp) return -1;

    if (dest == TBFT_ALL_REPLICAS) {
        if (udp->use_multicast) {
            int ret = sendto(udp->sock, buf, len, 0,
                             (struct sockaddr *)&udp->mcast_addr,
                             sizeof(udp->mcast_addr));
            if (ret < 0) {
                ESP_LOGE(TAG, "sendto(multicast) failed: %d", errno);
            }
            return ret;
        } else {
            /* Unicast to each replica */
            int sent = 0;
            int limit = udp->num_replicas > 0 ? udp->num_replicas : udp->num_nodes;
            for (int i = 0; i < limit; i++) {
                if (!udp->peer_valid[i]) continue;
                struct sockaddr_in addr = {
                    .sin_family      = AF_INET,
                    .sin_addr.s_addr = tbft_addr_udp_ip(udp->peers[i]),
                    .sin_port        = tbft_addr_udp_port(udp->peers[i]),
                };
                int r = sendto(udp->sock, buf, len, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
                if (r > 0) sent += r;
            }
            return sent;
        }
    }

    /* Unicast to single destination */
    if (dest < 0 || dest >= udp->num_nodes || !udp->peer_valid[dest]) {
        ESP_LOGE(TAG, "send: peer %d not registered", dest);
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = tbft_addr_udp_ip(udp->peers[dest]),
        .sin_port        = tbft_addr_udp_port(udp->peers[dest]),
    };
    int ret = sendto(udp->sock, buf, len, 0,
                     (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "sendto(%d) failed: %d", dest, errno);
    }
    return ret;
}

int tbft_transport_recv(tbft_transport_t *t, void *buf, size_t buf_len,
                        tbft_node_id_t *src_id)
{
    tbft_udp_t *udp = (tbft_udp_t *)t;
    if (!udp) return -1;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    int n = recvfrom(udp->sock, buf, buf_len, 0,
                     (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            return 0; /* nothing available */
        }
        ESP_LOGE(TAG, "recvfrom failed: %d", errno);
        return -1;
    }

    /* Try to identify sender by matching IP:port */
    if (src_id) {
        *src_id = -1;
        for (int i = 0; i < udp->num_nodes; i++) {
            if (!udp->peer_valid[i]) continue;
            if (tbft_addr_udp_ip(udp->peers[i])  == from.sin_addr.s_addr
                  && tbft_addr_udp_port(udp->peers[i]) == from.sin_port) {
                *src_id = (tbft_node_id_t)i;
                break;
            }
        }
    }

    return n;
}
