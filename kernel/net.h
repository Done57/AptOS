#ifndef APTOS_NET_H
#define APTOS_NET_H

/* Tiny userland-free network stack: ethernet + ARP + IPv4 + ICMP echo +
 * UDP, with DHCP and DNS clients riding on UDP.  Synchronous: `ping` and
 * friends run their own poll loop until the answer arrives or times out.
 * No sockets, no TCP - a toy OS does not need them.
 */

#include "types.h"

#define NET_IP_LEN      4
#define NET_MAC_LEN     6

typedef struct {
    u8  ip[NET_IP_LEN];        /* 0.0.0.0 = unset */
    u8  mask[NET_IP_LEN];      /* 0.0.0.0 = unset */
    u8  gw[NET_IP_LEN];        /* 0.0.0.0 = unset */
    u8  dhcp_server[NET_IP_LEN];
    char domain[64];
    u8  dns[NET_IP_LEN];
    u32 lease_seconds;
} net_cfg_t;

void net_init(void);                       /* after e1000_init() */

const net_cfg_t *net_cfg(void);
int  net_link_up(void);

/* blocking helpers (return -1 on timeout) */
int  net_dhcp_discover(u32 timeout_ms);    /* fills net_cfg_t */
int  net_ping(const u8 ip[NET_IP_LEN], u32 timeout_ms, u32 *rtt_ms);
int  net_dns_resolve(const char *name, u8 out_ip[NET_IP_LEN], u32 timeout_ms);

/* string helpers */
void net_ip_to_str(const u8 ip[4], char *buf, int buflen);
int  net_str_to_ip(const char *s, u8 out[4]);

#endif
