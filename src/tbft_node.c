#include "tbft_node.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "tbft_node";

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
 * Lifecycle
 * -------------------------------------------------------------------------- */

int tbft_node_init(tbft_node_t *node, tbft_node_id_t node_id,
                   int f, int num_nodes,
                   const char *mcast_ip, int64_t auth_timeout_us,
                   uint16_t port)
{
    memset(node, 0, sizeof(*node));

    node->node_id       = node_id;
    node->max_faulty    = f;
    node->num_replicas  = 3 * f + 1;
    node->threshold     = 2 * f + 1;
    node->num_principals = num_nodes;
    node->view           = 0;
    node->cur_primary    = 0;
    node->auth_timeout_us = auth_timeout_us;

#ifdef CONFIG_TBFT_DISABLE_MULTICAST
    node->use_multicast = false;
#else
    node->use_multicast = (mcast_ip != NULL);
#endif

    node->sock = socket_open(port, node->use_multicast, mcast_ip,
                             &node->mcast_addr);
    if (node->sock < 0) {
        return -1;
    }

    /* Initialise authentication freshness timer */
    esp_err_t err = tbft_itimer_init(&node->atimer, NULL, node, "tbft_auth");
    if (err != ESP_OK) {
        close(node->sock);
        return -1;
    }

    ESP_LOGI(TAG, "node %d init: f=%d n=%d quorum=%d port=%d",
             node_id, f, node->num_replicas, node->threshold, port);
    return 0;
}

void tbft_node_free(tbft_node_t *node)
{
    tbft_itimer_free(&node->atimer);
    if (node->sock >= 0) {
        close(node->sock);
        node->sock = -1;
    }
    for (int i = 0; i < node->num_principals; i++) {
        if (node->principals[i]) {
            tbft_principal_free(node->principals[i]);
            node->principals[i] = NULL;
        }
    }
}

/* --------------------------------------------------------------------------
 * Network I/O
 * -------------------------------------------------------------------------- */

int tbft_node_send(tbft_node_t *node, const void *buf, size_t len,
                   tbft_node_id_t dest)
{
    if (dest == TBFT_ALL_REPLICAS) {
        if (node->use_multicast) {
            int ret = sendto(node->sock, buf, len, 0,
                             (struct sockaddr *)&node->mcast_addr,
                             sizeof(node->mcast_addr));
            if (ret < 0) {
                ESP_LOGE(TAG, "sendto(multicast) failed: %d", errno);
            }
            return ret;
        } else {
            /* Unicast to each replica */
            int sent = 0;
            for (int i = 0; i < node->num_replicas; i++) {
                if (i == node->node_id) continue;
                tbft_principal_t *p = node->principals[i];
                if (!p) continue;
                struct sockaddr_in addr = {
                    .sin_family      = AF_INET,
                    .sin_addr.s_addr = p->addr.ip,
                    .sin_port        = p->addr.port,
                };
                int r = sendto(node->sock, buf, len, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
                if (r > 0) sent = r;
            }
            return sent;
        }
    }

    /* Unicast to single destination */
    tbft_principal_t *p = node->principals[dest];
    if (!p) {
        ESP_LOGE(TAG, "send: unknown principal %d", dest);
        return -1;
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = p->addr.ip,
        .sin_port        = p->addr.port,
    };
    int ret = sendto(node->sock, buf, len, 0,
                     (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        ESP_LOGE(TAG, "sendto(%d) failed: %d", dest, errno);
    }
    return ret;
}

int tbft_node_recv(tbft_node_t *node, void *buf, tbft_node_id_t *src_id)
{
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    int n = recvfrom(node->sock, buf, TBFT_MAX_MESSAGE_SIZE, 0,
                     (struct sockaddr *)&from, &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0; /* nothing available */
        }
        ESP_LOGE(TAG, "recvfrom failed: %d", errno);
        return -1;
    }
    if (n < (int)sizeof(tbft_msg_hdr_t)) {
        return 0; /* truncated */
    }

    /* Try to identify sender by matching IP:port */
    if (src_id) {
        *src_id = -1;
        for (int i = 0; i < node->num_principals; i++) {
            tbft_principal_t *p = node->principals[i];
            if (p && p->addr.ip   == from.sin_addr.s_addr
                  && p->addr.port == from.sin_port) {
                *src_id = p->id;
                break;
            }
        }
    }

    return n;
}

/* --------------------------------------------------------------------------
 * Authenticator generation / verification
 * -------------------------------------------------------------------------- */

void tbft_node_gen_auth(tbft_node_t *node, const void *msg, size_t msg_len,
                        tbft_auth_t *auth)
{
    int slot = 0;
    for (int i = 0; i < node->num_replicas; i++) {
        if (i == node->node_id) continue;
        tbft_principal_t *p = node->principals[i];
        if (p) {
            tbft_principal_gen_mac_out(p, msg, msg_len, &auth->slots[slot]);
        } else {
            memset(&auth->slots[slot], 0, sizeof(auth->slots[slot]));
        }
        slot++;
    }
}

bool tbft_node_verify_auth(tbft_node_t *node, tbft_node_id_t sender_id,
                           const void *msg, size_t msg_len,
                           const tbft_mac_t *mac)
{
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_mac_in(p, msg, msg_len, mac);
}

/* --------------------------------------------------------------------------
 * RSA signature path
 * -------------------------------------------------------------------------- */

int tbft_node_gen_sig(tbft_node_t *node, const void *msg, size_t msg_len,
                      tbft_sig_t *sig)
{
    if (!node->local_principal) return -1;
    return tbft_principal_sign(node->local_principal, msg, msg_len, sig);
}

bool tbft_node_verify_sig(tbft_node_t *node, tbft_node_id_t sender_id,
                          const void *msg, size_t msg_len,
                          const tbft_sig_t *sig)
{
    tbft_principal_t *p = node->principals[sender_id];
    if (!p) return false;
    return tbft_principal_verify_sig(p, msg, msg_len, sig);
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

tbft_req_id_t tbft_node_new_rid(tbft_node_t *node)
{
    node->rid_counter++;
    return ((tbft_req_id_t)(uint64_t)node->node_id << 48) | node->rid_counter;
}
