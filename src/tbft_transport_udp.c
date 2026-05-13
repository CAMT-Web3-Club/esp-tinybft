/**
 * @file tbft_transport_udp.c
 * @brief UDP transport backend — lwIP sockets.
 *
 * Extracted from the original tbft_node.c socket logic.
 */

#include "tbft_transport.h"
#include "esp_log.h"
#include "esp_netif.h"
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
    int          local_id;       /* auto-detected via local netif IP; -1 if unknown */
} tbft_udp_t;

/* sendto with bounded retry on transient TX-queue stalls.
 *
 * The socket is non-blocking (O_NONBLOCK set in socket_open), so a
 * momentarily-full WiFi MAC / lwIP TX queue causes sendto to return
 * EAGAIN/EWOULDBLOCK. Without retry, replies fired by 4 replicas at
 * roughly the same instant race for the same queue and most fail
 * silently — observed as "only one replica's reply reaches the client".
 * Hard errors (e.g. ENETUNREACH) are returned immediately. */
static int udp_sendto_retry(int sock, const void *buf, size_t len,
                            const struct sockaddr_in *addr)
{
    const int MAX_RETRIES = 3;
    int last_errno = 0;
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int r = sendto(sock, buf, len, 0,
                       (const struct sockaddr *)addr, sizeof(*addr));
        if (r >= 0) return r;
        last_errno = errno;
        if (last_errno != EAGAIN && last_errno != EWOULDBLOCK) break;
        if (attempt + 1 < MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(5 * (attempt + 1)));
        }
    }
    errno = last_errno;
    return -1;
}

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
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        ESP_LOGE(TAG, "setsockopt(SO_REUSEADDR) failed: %d", errno);
        close(sock);
        return -1;
    }

    /* Set non-blocking.  Must not silently continue on failure: if the
     * socket stays blocking, tbft_transport_recv blocks forever and the
     * BFT main loop hangs.  Bail out and let the caller retry. */
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        ESP_LOGE(TAG, "fcntl(F_GETFL) failed: %d", errno);
        close(sock);
        return -1;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(TAG, "fcntl(F_SETFL) failed: %d", errno);
        close(sock);
        return -1;
    }

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
        /* Validate multicast IP string before use */
        struct in_addr mcast_addr;
        if (inet_pton(AF_INET, mcast_ip, &mcast_addr) != 1) {
            ESP_LOGE(TAG, "invalid multicast IP: %s", mcast_ip);
            close(sock);
            return -1;
        }

        /* Join multicast group */
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = mcast_addr.s_addr;
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP failed: %d", errno);
            close(sock);
            return -1;
        }

        /* Set multicast TTL */
        uint8_t ttl = 32;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
            ESP_LOGE(TAG, "setsockopt(IP_MULTICAST_TTL) failed: %d", errno);
            close(sock);
            return -1;
        }

        /* M6 FIX: Disable multicast loopback to prevent receiving our own
         * messages. Without this, every broadcast is echoed back to the
         * sender, wasting CPU cycles processing self-sent BFT messages. */
        uint8_t loop = 0;
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
            ESP_LOGW(TAG, "setsockopt(IP_MULTICAST_LOOP) failed: %d", errno);
            /* Non-fatal: continue without disabling loopback */
        }

        /* Store multicast destination address */
        memset(mcast_addr_out, 0, sizeof(*mcast_addr_out));
        mcast_addr_out->sin_family      = AF_INET;
        mcast_addr_out->sin_addr        = mcast_addr;
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
    udp->local_id     = -1;

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
        /* L1 FIX: Leave multicast group before closing socket to ensure
         * IGMP membership is properly cleaned up. */
        if (udp->use_multicast) {
            struct ip_mreq mreq;
            mreq.imr_multiaddr = udp->mcast_addr.sin_addr;
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            setsockopt(udp->sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       &mreq, sizeof(mreq));
        }
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

    /* Auto-detect local_id by matching the peer's IP against the WiFi STA
     * netif IP. Mirrors the ESP-NOW backend (which matches by eFuse MAC).
     * Used by the broadcast-unicast fallback to avoid sendto'ing to
     * ourselves via UDP loopback, which would waste a TX-queue slot. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK
            && ip_info.ip.addr != 0
            && ip_info.ip.addr == tbft_addr_udp_ip(*addr)) {
            udp->local_id = node_id;
        }
    }
}

int tbft_transport_send(tbft_transport_t *t, const void *buf, size_t len,
                        tbft_node_id_t dest)
{
    tbft_udp_t *udp = (tbft_udp_t *)t;
    if (!udp) return -1;

    if (dest == TBFT_ALL_REPLICAS) {
        if (udp->use_multicast) {
            int ret = udp_sendto_retry(udp->sock, buf, len, &udp->mcast_addr);
            if (ret < 0) {
                ESP_LOGE(TAG, "sendto(multicast) failed: %d", errno);
            }
            return ret;
        } else {
            /* Unicast to each replica */
            int ok = 0;
            int limit = udp->num_replicas > 0 ? udp->num_replicas : udp->num_nodes;
            for (int i = 0; i < limit; i++) {
                if (i == udp->local_id) continue;   /* skip self via loopback */
                if (!udp->peer_valid[i]) continue;
                struct sockaddr_in addr = {
                    .sin_family      = AF_INET,
                    .sin_addr.s_addr = tbft_addr_udp_ip(udp->peers[i]),
                    .sin_port        = tbft_addr_udp_port(udp->peers[i]),
                };
                int r = udp_sendto_retry(udp->sock, buf, len, &addr);
                if (r >= 0) ok++;
            }
            return (ok > 0) ? (int)len : -1;
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
    int ret = udp_sendto_retry(udp->sock, buf, len, &addr);
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
            /* HIGH FIX H4: Return immediately without delay.
             * The previous vTaskDelay(10) capped the entire BFT protocol to
             * 100 receive attempts/second, significantly increasing latency
             * and potentially triggering timeout cascades. */
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
            if (tbft_addr_udp_ip(udp->peers[i]) == from.sin_addr.s_addr
                  && tbft_addr_udp_port(udp->peers[i]) == from.sin_port) {
                *src_id = (tbft_node_id_t)i;
                break;
            }
        }
    }

    return n;
}
