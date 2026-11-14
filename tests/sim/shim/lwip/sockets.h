/*
 * lwIP BSD-socket API on the host's BSD sockets.
 *
 * Three calls are routed through simulator wrappers (shim_src/sim_net.c):
 *   bind()   - INADDR_ANY is replaced by the node's own loopback address
 *              (127.0.0.x from the config file), so every node has its own
 *              IP as on real boards and replies come from that address;
 *   sendto() - fault injection: env SIM_LOSS drops a datagram with that
 *              probability (SIM_SEED makes the stream reproducible), plus
 *              optional partitions (SIM_BLOCK) and counters;
 *   select() - releases the simulator's task lock while blocked.
 * Everything else is the host implementation.
 */
#pragma once

/* ESP-IDF's lwIP headers pull these in (lwipopts.h, arch/cc.h);
 * tbft_transport_udp.c relies on that for snprintf() and calloc()/free(). */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int     sim_bind(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t sim_sendto(int fd, const void *buf, size_t len, int flags,
                   const struct sockaddr *to, socklen_t tolen);
int     sim_select(int nfds, fd_set *readfds, fd_set *writefds,
                   fd_set *exceptfds, struct timeval *timeout);

#define bind(...)   sim_bind(__VA_ARGS__)
#define sendto(...) sim_sendto(__VA_ARGS__)
#define select(...) sim_select(__VA_ARGS__)
