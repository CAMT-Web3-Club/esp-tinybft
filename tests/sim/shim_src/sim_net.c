/*
 * lwIP/esp_netif replacement for the host simulator.
 *
 *  - esp_netif "WIFI_STA_DEF" reports the node's own address (set by the sim
 *    main from the config file), so the unmodified UDP transport detects its
 *    local id and skips sending to itself, as on a board.
 *  - bind(INADDR_ANY) is narrowed to that address: every node owns a
 *    distinct 127.0.0.x, and datagrams it sends carry that source address.
 *  - sendto() drops datagrams with probability SIM_LOSS (0..1).  With
 *    SIM_LOSS_PER_FRAGMENT=1 the probability applies per 1472-byte IP
 *    fragment (a Wi-Fi link loses the whole datagram if any fragment is
 *    lost), otherwise per datagram.  SIM_LOSS_FROM_S / SIM_LOSS_UNTIL_S limit
 *    the loss to a window of run time (seconds since the run start).
 *  - select() drops the simulator task lock while it blocks.
 */
#define _GNU_SOURCE
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "sim_rt.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* The wrappers below must call the host functions, not themselves. */
#undef bind
#undef sendto
#undef select

static struct in_addr g_local_ip;
static bool           g_local_ip_set;

struct esp_netif_obj { int dummy; };
static struct esp_netif_obj g_sta_netif;

static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;
static sim_net_stats_t g_stats;

static double g_loss = -1.0;      /* lazily read from the environment */
static bool   g_loss_per_frag;
static double g_loss_from_s, g_loss_until_s;

int sim_net_set_local_ip(const char *ip)
{
    struct in_addr a;
    if (!ip || inet_pton(AF_INET, ip, &a) != 1) return -1;
    g_local_ip = a;
    g_local_ip_set = true;
    return 0;
}

esp_err_t esp_netif_init(void) { return ESP_OK; }

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key)
{
    if (!g_local_ip_set || !if_key) return nullptr;
    if (strcmp(if_key, "WIFI_STA_DEF") != 0) return nullptr;
    return &g_sta_netif;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *info)
{
    if (!netif || !info || !g_local_ip_set) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    info->ip.addr = g_local_ip.s_addr;
    info->netmask.addr = htonl(0xff000000u);
    info->gw.addr = htonl(0x7f000001u);
    return ESP_OK;
}

int sim_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (g_local_ip_set && addr && addr->sa_family == AF_INET &&
        len >= (socklen_t)sizeof(struct sockaddr_in)) {
        struct sockaddr_in a;
        memcpy(&a, addr, sizeof(a));
        if (a.sin_addr.s_addr == htonl(INADDR_ANY)) {
            a.sin_addr = g_local_ip;
            return bind(fd, (const struct sockaddr *)&a, sizeof(a));
        }
    }
    return bind(fd, addr, len);
}

__attribute__((constructor)) static void loss_init(void)
{
    if (g_loss >= 0.0) return;
    double p = sim_rt_env_double("SIM_LOSS", 0.0);
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    g_loss_per_frag = sim_rt_env_int("SIM_LOSS_PER_FRAGMENT", 0) != 0;
    g_loss_from_s   = sim_rt_env_double("SIM_LOSS_FROM_S", 0.0);
    g_loss_until_s  = sim_rt_env_double("SIM_LOSS_UNTIL_S", 1e18);
    g_loss = p;
}

static bool drop_this(size_t len)
{
    loss_init();
    if (g_loss <= 0.0) return false;
    double now = sim_rt_run_s();
    if (now < g_loss_from_s || now >= g_loss_until_s) return false;
    int frags = 1;
    if (g_loss_per_frag) {
        frags = (int)((len + 1471) / 1472);
        if (frags < 1) frags = 1;
    }
    for (int i = 0; i < frags; i++) {
        if (sim_rt_rand_unit(SIM_RNG_NET) < g_loss) return true;
    }
    return false;
}

ssize_t sim_sendto(int fd, const void *buf, size_t len, int flags,
                   const struct sockaddr *to, socklen_t tolen)
{
    if (drop_this(len)) {
        pthread_mutex_lock(&g_net_lock);
        g_stats.dropped_loss++;
        pthread_mutex_unlock(&g_net_lock);
        return (ssize_t)len;   /* lost on the air: the sender cannot tell */
    }
    ssize_t r = sendto(fd, buf, len, flags, to, tolen);
    if (r >= 0) {
        pthread_mutex_lock(&g_net_lock);
        g_stats.sent++;
        pthread_mutex_unlock(&g_net_lock);
    }
    return r;
}

int sim_select(int nfds, fd_set *readfds, fd_set *writefds,
               fd_set *exceptfds, struct timeval *timeout)
{
    bool h = sim_blocking_begin();
    int r = select(nfds, readfds, writefds, exceptfds, timeout);
    sim_blocking_end(h);
    return r;
}

void sim_net_get_stats(sim_net_stats_t *out)
{
    pthread_mutex_lock(&g_net_lock);
    *out = g_stats;
    pthread_mutex_unlock(&g_net_lock);
}
