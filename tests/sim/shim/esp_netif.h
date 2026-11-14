/*
 * esp_netif subset.  The "WIFI_STA_DEF" interface exists once the sim main
 * has called sim_net_set_local_ip(); its address is the node's own loopback
 * address from the config file (each node gets its own 127.0.0.x), so the
 * UDP transport's local-id detection and the parser's IP auto-detection
 * behave as on a board with a DHCP lease.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct esp_netif_obj esp_netif_t;

typedef struct { uint32_t addr; } esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

#define esp_ip4_addr_get_byte(ipaddr, idx) (((const uint8_t *)(&(ipaddr)->addr))[idx])
#define esp_ip4_addr1(ipaddr) esp_ip4_addr_get_byte(ipaddr, 0)
#define esp_ip4_addr2(ipaddr) esp_ip4_addr_get_byte(ipaddr, 1)
#define esp_ip4_addr3(ipaddr) esp_ip4_addr_get_byte(ipaddr, 2)
#define esp_ip4_addr4(ipaddr) esp_ip4_addr_get_byte(ipaddr, 3)
#define esp_ip4_addr1_16(ipaddr) ((uint16_t)esp_ip4_addr1(ipaddr))
#define esp_ip4_addr2_16(ipaddr) ((uint16_t)esp_ip4_addr2(ipaddr))
#define esp_ip4_addr3_16(ipaddr) ((uint16_t)esp_ip4_addr3(ipaddr))
#define esp_ip4_addr4_16(ipaddr) ((uint16_t)esp_ip4_addr4(ipaddr))
#define IP2STR(ipaddr) esp_ip4_addr1_16(ipaddr), esp_ip4_addr2_16(ipaddr), \
                       esp_ip4_addr3_16(ipaddr), esp_ip4_addr4_16(ipaddr)
#define IPSTR "%d.%d.%d.%d"

esp_err_t    esp_netif_init(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *if_key);
esp_err_t    esp_netif_get_ip_info(esp_netif_t *esp_netif,
                                   esp_netif_ip_info_t *ip_info);

/* Simulator extension (shim_src/sim_net.c). */
int  sim_net_set_local_ip(const char *ip);   /* 0 ok, -1 bad address */
