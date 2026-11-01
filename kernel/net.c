#include "net.h"
#include "e1000.h"
#include "klog.h"
#include "timer.h"
#include "util.h"

#define ETHTYPE_ARP  0x0806
#define ETHTYPE_IPV4 0x0800

#define PROTO_ICMP 1
#define PROTO_UDP  17

#define ARP_REQUEST 1
#define ARP_REPLY   2

#define ETH_HDR_LEN 14
#define ARP_PKT_LEN 28
#define IP_HDR_LEN  20
#define UDP_HDR_LEN 8
#define ICMP_ECHO_HDR_LEN 8

#define DHCP_SERVER 67
#define DHCP_CLIENT 68
#define DNS_PORT    53

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define NET_BUF_LEN 1600

/* ------------------------------------------------------------------ */
typedef struct {
    u8  dst[NET_MAC_LEN];
    u8  src[NET_MAC_LEN];
    u16 type;                 /* big endian on the wire */
} eth_hdr_t;

typedef struct {
    u16 htype;
    u16 ptype;
    u8  hlen;
    u8  plen;
    u16 op;
    u8  sha[NET_MAC_LEN];
    u8  spa[4];
    u8  tha[NET_MAC_LEN];
    u8  tpa[4];
} arp_pkt_t;

typedef struct {
    u8  ver_ihl;
    u8  tos;
    u16 total_len;
    u16 id;
    u16 flags_frag;
    u8  ttl;
    u8  proto;
    u16 checksum;
    u8  src[4];
    u8  dst[4];
} ip_hdr_t;

typedef struct {
    u16 src_port;
    u16 dst_port;
    u16 len;
    u16 checksum;
} udp_hdr_t;

typedef struct {
    u8  type;
    u8  code;
    u16 checksum;
    u16 id;
    u16 seq;
} icmp_hdr_t;

/* ------------------------------------------------------------------ */
static net_cfg_t cfg;
static u8 my_mac[NET_MAC_LEN];
static int have_ip;
static u16 ip_id;

/* Small ARP cache: we only ever talk to the DHCP server / DNS / one ping
 * target, so four entries are plenty. */
#define ARP_SLOTS 4
static struct {
    u8 ip[4];
    u8 mac[NET_MAC_LEN];
    int used;
} arp_cache[ARP_SLOTS];

static u16 be16(u16 v)
{
    return (u16)((v >> 8) | (v << 8));
}

/* ------------------------------------------------------------------ */
/* Returns the one's complement checksum in HOST byte order, ready to be
 * stored into a u16 struct field (the compiler then emits it big-endian
 * on the wire).  The sum itself walks the data as big-endian words. */
static u16 checksum(const u8 *data, u32 len, u32 seed)
{
    u32 sum = seed;

    for (u32 i = 0; i + 1 < len; i += 2)
        sum += (u32)data[i] << 8 | data[i + 1];
    if (len & 1)
        sum += (u32)data[len - 1] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(((~sum & 0xFFFF) >> 8) | ((~sum & 0xFF) << 8));
}

static int ip_eq(const u8 a[4], const u8 b[4])
{
    return memcmp(a, b, 4) == 0;
}

static void ip_copy(u8 dst[4], const u8 src[4])
{
    memcpy(dst, src, 4);
}

/* ------------------------------------------------------------------ */
static int arp_lookup(const u8 ip[4], u8 mac_out[NET_MAC_LEN])
{
    for (int i = 0; i < ARP_SLOTS; i++)
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip)) {
            memcpy(mac_out, arp_cache[i].mac, NET_MAC_LEN);
            return 0;
        }
    return -1;
}

static void arp_learn(const u8 ip[4], const u8 mac[NET_MAC_LEN])
{
    int slot = -1;

    if (ip[0] == 0)
        return;
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_cache[i].used && ip_eq(arp_cache[i].ip, ip)) {
            slot = i;
            break;
        }
        if (!arp_cache[i].used)
            slot = i;
    }
    if (slot < 0)
        slot = 0;
    ip_copy(arp_cache[slot].ip, ip);
    memcpy(arp_cache[slot].mac, mac, NET_MAC_LEN);
    arp_cache[slot].used = 1;
}

static int arp_request(const u8 ip[4])
{
    u8 frame[ETH_HDR_LEN + ARP_PKT_LEN];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + ETH_HDR_LEN);

    memset(eth->dst, 0xFF, NET_MAC_LEN);
    memcpy(eth->src, my_mac, NET_MAC_LEN);
    eth->type = be16(ETHTYPE_ARP);

    arp->htype = be16(1);
    arp->ptype = be16(ETHTYPE_IPV4);
    arp->hlen = NET_MAC_LEN;
    arp->plen = 4;
    arp->op = be16(ARP_REQUEST);
    memcpy(arp->sha, my_mac, NET_MAC_LEN);
    ip_copy(arp->spa, cfg.ip);
    memset(arp->tha, 0, NET_MAC_LEN);
    ip_copy(arp->tpa, ip);

    return e1000_send(frame, sizeof(frame));
}

/* Returns 0 and fills mac_out once the answer arrives. */
static int arp_resolve(const u8 ip[4], u8 mac_out[NET_MAC_LEN], u32 timeout_ms)
{
    u8 buf[NET_BUF_LEN];
    u32 start = timer_uptime_ms();

    if (arp_lookup(ip, mac_out) == 0)
        return 0;

    arp_request(ip);
    while (timer_uptime_ms() - start < timeout_ms) {
        int n = e1000_recv(buf, sizeof(buf));
        if (n >= (int)(ETH_HDR_LEN + ARP_PKT_LEN)) {
            eth_hdr_t *eth = (eth_hdr_t *)buf;
            arp_pkt_t *arp = (arp_pkt_t *)(buf + ETH_HDR_LEN);

            if (eth->type == be16(ETHTYPE_ARP) && arp->op == be16(ARP_REPLY) &&
                ip_eq(arp->spa, ip)) {
                arp_learn(arp->spa, arp->sha);
                memcpy(mac_out, arp->sha, NET_MAC_LEN);
                return 0;
            }
        }
        for (volatile int i = 0; i < 2000; i++)
            ;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Is dst on our own subnet?  Without a mask (DHCP not done) assume
 * everything is local, which keeps the pre-DHCP broadcast paths working. */
static int ip_is_local(const u8 ip[4])
{
    int have_mask = 0;

    for (int i = 0; i < 4; i++)
        if (cfg.mask[i]) have_mask = 1;
    if (!have_mask)
        return 1;
    for (int i = 0; i < 4; i++)
        if ((ip[i] & cfg.mask[i]) != (cfg.ip[i] & cfg.mask[i]))
            return 0;
    return 1;
}

/* The next hop we must ARP for in order to reach `dst`: the destination
 * itself when it is on-link, otherwise the gateway.  Off-subnet hosts
 * (e.g. 208.67.222.222) never answer ARP directly - only the router does. */
static int ip_next_hop(const u8 dst[4], u8 hop_out[4])
{
    if (dst[0] == 255 || dst[0] == 0 || ip_is_local(dst)) {
        ip_copy(hop_out, dst);
        return 0;
    }
    if (!cfg.gw[0] && !cfg.gw[1] && !cfg.gw[2] && !cfg.gw[3])
        return -1;                     /* off-net with no route out */
    ip_copy(hop_out, cfg.gw);
    return 0;
}

static void ip_to_eth_dst(const u8 ip[4], u8 eth_dst[NET_MAC_LEN])
{
    u8 hop[4];

    if (ip[0] == 255 || ip[0] == 0)
        memset(eth_dst, 0xFF, NET_MAC_LEN);
    else if (ip_next_hop(ip, hop) == 0 && arp_lookup(hop, eth_dst) == 0)
        ;                                      /* routed to hop's MAC */
    else
        memset(eth_dst, 0xFF, NET_MAC_LEN);    /* best effort */
}

static int ip_send(u8 proto, const u8 dst_ip[4], const void *payload, u32 len)
{
    u8 frame[NET_BUF_LEN];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ip_hdr_t *ip = (ip_hdr_t *)(frame + ETH_HDR_LEN);
    u32 plen = IP_HDR_LEN + len;

    if (plen + ETH_HDR_LEN > sizeof(frame))
        return -1;

    ip_to_eth_dst(dst_ip, eth->dst);
    memcpy(eth->src, my_mac, NET_MAC_LEN);
    eth->type = be16(ETHTYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = be16((u16)plen);
    ip->id = be16(++ip_id);
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip_copy(ip->src, cfg.ip);
    ip_copy(ip->dst, dst_ip);
    ip->checksum = checksum((u8 *)ip, IP_HDR_LEN, 0);

    memcpy(frame + ETH_HDR_LEN + IP_HDR_LEN, payload, len);
    return e1000_send(frame, ETH_HDR_LEN + plen);
}

/* Handle one incoming IPv4 datagram; returns the payload length if it was
 * for us (buf is advanced past the IP header), 0 if not interesting. */
static int ip_input(u8 *frame, int framelen, u8 **payload_out, u8 *proto_out,
                    u8 src_out[4])
{
    ip_hdr_t *ip;
    u32 ihl, total;

    if (framelen < (int)(ETH_HDR_LEN + IP_HDR_LEN))
        return 0;
    ip = (ip_hdr_t *)(frame + ETH_HDR_LEN);
    if (ip->ver_ihl >> 4 != 4)
        return 0;

    ihl = (u32)(ip->ver_ihl & 0xF) * 4;
    if (ihl < IP_HDR_LEN)
        return 0;
    total = be16(ip->total_len);
    if (total < ihl || ETH_HDR_LEN + total > (u32)framelen)
        return 0;

    /* accept anything with our IP, plus the 255.255.255.255 broadcast that
     * DHCP answers with before we have an address */
    if (!have_ip && !ip_eq(ip->dst, cfg.ip) && !(ip->dst[0] == 255 &&
        ip->dst[1] == 255 && ip->dst[2] == 255 && ip->dst[3] == 255))
        return 0;
    if (have_ip && !ip_eq(ip->dst, cfg.ip) &&
        !(ip->dst[0] == 255 && ip->dst[1] == 255 &&
          ip->dst[2] == 255 && ip->dst[3] == 255))
        return 0;

    ip_copy(src_out, ip->src);
    *proto_out = ip->proto;
    *payload_out = frame + ETH_HDR_LEN + ihl;
    return (int)(total - ihl);
}

/* ------------------------------------------------------------------ */
static int udp_send(const u8 dst_ip[4], u16 sport, u16 dport,
                    const void *data, u32 len)
{
    u8 pkt[NET_BUF_LEN];
    udp_hdr_t *udp = (udp_hdr_t *)pkt;

    if (UDP_HDR_LEN + len > sizeof(pkt))
        return -1;
    udp->src_port = be16(sport);
    udp->dst_port = be16(dport);
    udp->len = be16((u16)(UDP_HDR_LEN + len));
    udp->checksum = 0;                 /* optional under IPv4 */
    memcpy(pkt + UDP_HDR_LEN, data, len);
    return ip_send(PROTO_UDP, dst_ip, pkt, UDP_HDR_LEN + len);
}

/* ------------------------------------------------------------------ */
/* DHCP: one DISCOVER, collect OFFERs, REQUEST the first offer, wait ACK. */

#define DHCP_OPT_END     255
#define DHCP_OPT_PAD       0
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_HOSTNAME  12
#define DHCP_OPT_REQ_IP    50
#define DHCP_OPT_LEASE     51
#define DHCP_OPT_MSGTYPE   53
#define DHCP_OPT_CLIENT_ID 61
#define DHCP_OPT_PARAMS    55

typedef struct {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u8  ciaddr[4], yiaddr[4], siaddr[4], giaddr[4];
    u8  chaddr[16];
    char sname[64];
    char file[128];
    u32 magic;                 /* 0x63825363 */
    u8  options[312];
} dhcp_pkt_t;

static u32 dhcp_xid;

static int dhcp_parse_offer(const dhcp_pkt_t *p, u32 plen)
{
    const u8 *o = p->options;
    u32 left = plen < sizeof(p->options) ? plen : sizeof(p->options);

    ip_copy(cfg.ip, p->yiaddr);
    ip_copy(cfg.dhcp_server, p->siaddr);
    memset(cfg.mask, 0, 4);
    memset(cfg.gw, 0, 4);
    memset(cfg.dns, 0, 4);
    cfg.lease_seconds = 0;
    cfg.domain[0] = 0;

    while (left >= 2 && *o != DHCP_OPT_END) {
        u8 code = o[0], len = o[1];
        if (code == DHCP_OPT_PAD) {
            o++;
            left--;
            continue;
        }
        if ((u32)(o - p->options) + 2 + len > plen)
            break;
        switch (code) {
        case DHCP_OPT_SUBNET:
            if (len == 4) ip_copy(cfg.mask, o + 2);
            break;
        case DHCP_OPT_ROUTER:
            if (len >= 4) ip_copy(cfg.gw, o + 2);
            break;
        case DHCP_OPT_DNS:
            if (len >= 4) ip_copy(cfg.dns, o + 2);
            break;
        case DHCP_OPT_LEASE:
            if (len == 4) {
                u32 s = ((u32)o[2] << 24) | ((u32)o[3] << 16) |
                        ((u32)o[4] << 8) | o[5];
                cfg.lease_seconds = s;
            }
            break;
        case DHCP_OPT_HOSTNAME:
            {
                u32 n = len < sizeof(cfg.domain) - 1 ? len : sizeof(cfg.domain) - 1;
                memcpy(cfg.domain, o + 2, n);
                cfg.domain[n] = 0;
            }
            break;
        }
        o += 2 + len;
        left -= 2 + len;
    }
    return 0;
}

static int dhcp_build(u8 msgtype, const u8 req_ip[4], dhcp_pkt_t *p)
{
    memset(p, 0, sizeof(*p));
    p->op = 1;                       /* BOOTREQUEST */
    p->htype = 1;
    p->hlen = NET_MAC_LEN;
    p->xid = dhcp_xid;
    p->flags = be16(0x8000);         /* broadcast answers, please */
    memcpy(p->chaddr, my_mac, NET_MAC_LEN);
    /* magic cookie 0x63825363 (little-endian store -> 63 82 53 63) */
    p->magic = 0x63825363u;
    {
        u8 *o = p->options;
        *o++ = DHCP_OPT_MSGTYPE; *o++ = 1; *o++ = msgtype;
        *o++ = DHCP_OPT_CLIENT_ID; *o++ = 7; *o++ = 1;
        memcpy(o, my_mac, NET_MAC_LEN); o += NET_MAC_LEN;
        *o++ = DHCP_OPT_PARAMS; *o++ = 4;
        *o++ = DHCP_OPT_SUBNET; *o++ = DHCP_OPT_ROUTER;
        *o++ = DHCP_OPT_DNS; *o++ = DHCP_OPT_LEASE;
        if (req_ip) {
            *o++ = DHCP_OPT_REQ_IP; *o++ = 4;
            memcpy(o, req_ip, 4); o += 4;
        }
        *o++ = DHCP_OPT_END;
    }
    return 0;
}

static int dhcp_send(u8 msgtype, const u8 req_ip[4], const u8 server[4])
{
    static dhcp_pkt_t pkt;
    dhcp_build(msgtype, req_ip, &pkt);
    if (server && (server[0] || server[1] || server[2] || server[3]))
        return udp_send(server, DHCP_CLIENT, DHCP_SERVER, &pkt, sizeof(pkt));
    {
        u8 bcast[4] = { 255, 255, 255, 255 };
        return udp_send(bcast, DHCP_CLIENT, DHCP_SERVER, &pkt, sizeof(pkt));
    }
}

/* Pump the NIC until a DHCP packet of the wanted type arrives (or timeout).
 * Returns 1 when seen; offer fills cfg from the packet. */
static int dhcp_wait(u8 want_type, u32 timeout_ms, int *got_ack)
{
    u8 buf[NET_BUF_LEN];
    u32 start = timer_uptime_ms();

    *got_ack = 0;
    while (timer_uptime_ms() - start < timeout_ms) {
        int n = e1000_recv(buf, sizeof(buf));
        if (n > (int)(ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + 240)) {
            u8 *pl;
            u8 proto, src[4];
            int plen = ip_input(buf, n, &pl, &proto, src);

            if (plen > 0 && proto == PROTO_UDP) {
                udp_hdr_t *udp = (udp_hdr_t *)pl;
                if (be16(udp->dst_port) == DHCP_CLIENT) {
                    dhcp_pkt_t *dp = (dhcp_pkt_t *)(pl + UDP_HDR_LEN);
                    int dlen = plen - UDP_HDR_LEN;
                    u8 type = 0;
                    const u8 *o = dp->options;
                    u32 left = dlen > 240 ? 240 : (u32)dlen;

                    /* find the message type option */
                    while (left >= 2 && *o != DHCP_OPT_END) {
                        if (*o == DHCP_OPT_PAD) { o++; left--; continue; }
                        if (o[0] == DHCP_OPT_MSGTYPE && o[1] >= 1) {
                            type = o[2];
                            break;
                        }
                        left -= 2 + o[1];
                        o += 2 + o[1];
                    }
                    /* Some servers (incl. QEMU slirp) answer DISCOVER
                     * with a full ACK; accept any final answer. */
                    if (type == want_type || type == DHCP_ACK) {
                        dhcp_parse_offer(dp, (u32)dlen);
                        if (want_type == DHCP_ACK)
                            *got_ack = 1;
                        return 1;
                    }
                }
            }
        }
        for (volatile int i = 0; i < 3000; i++)
            ;
    }
    return 0;
}

int net_dhcp_discover(u32 timeout_ms)
{
    int got_ack;

    if (!e1000_present())
        return -1;
    if (dhcp_xid == 0)
        dhcp_xid = (u32)timer_ticks() * 2654435761u | 1u;
    dhcp_xid += 0x01010101u;

    have_ip = 0;
    memset(&cfg, 0, sizeof(cfg));

    /* DISCOVER -> OFFER */
    for (int attempt = 0; attempt < 2; attempt++) {
        dhcp_send(DHCP_DISCOVER, NULL, NULL);
        if (dhcp_wait(DHCP_OFFER, timeout_ms / 2, &got_ack))
            break;
    }
    if (!cfg.ip[0] && !cfg.ip[1] && !cfg.ip[2] && !cfg.ip[3])
        return -1;

    /* REQUEST -> ACK */
    for (int attempt = 0; attempt < 2; attempt++) {
        dhcp_send(DHCP_REQUEST, cfg.ip, cfg.dhcp_server);
        if (dhcp_wait(DHCP_ACK, timeout_ms / 2, &got_ack))
            break;
    }

    have_ip = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
int net_ping(const u8 ip[4], u32 timeout_ms, u32 *rtt_ms)
{
    u8 frame[NET_BUF_LEN];
    u8 mac[NET_MAC_LEN];
    icmp_hdr_t *icmp;
    u32 start;
    u16 ident = (u16)(timer_ticks() | 1);
    u16 seq = 1;

    if (!e1000_present())
        return -1;
    {
        u8 hop[4];

        if (ip_next_hop(ip, hop) != 0)
            return -4;                 /* off-subnet and no gateway */
        if (arp_resolve(hop, mac, timeout_ms / 2) != 0)
            return -2;                 /* no reply to ARP for the hop */
    }

    /* build ICMP echo request directly into an IP packet */
    {
        u8 pkt[ICMP_ECHO_HDR_LEN + 32];
        memset(pkt, 0, sizeof(pkt));
        icmp = (icmp_hdr_t *)pkt;
        icmp->type = 8;                    /* echo request */
        icmp->code = 0;
        icmp->id = be16(ident);
        icmp->seq = be16(seq);
        memcpy(pkt + ICMP_ECHO_HDR_LEN, "aptos-ping!", 11);
        icmp->checksum = checksum(pkt, sizeof(pkt), 0);

        start = timer_uptime_ms();
        if (ip_send(PROTO_ICMP, ip, pkt, sizeof(pkt)) != 0)
            return -3;
    }

    while (timer_uptime_ms() - start < timeout_ms) {
        int n = e1000_recv(frame, sizeof(frame));
        if (n > (int)(ETH_HDR_LEN + IP_HDR_LEN + ICMP_ECHO_HDR_LEN)) {
            u8 *pl;
            u8 proto, src[4];
            int plen = ip_input(frame, n, &pl, &proto, src);

            if (plen > (int)ICMP_ECHO_HDR_LEN && proto == PROTO_ICMP) {
                icmp_hdr_t *rx = (icmp_hdr_t *)pl;
                if (rx->type == 0 && be16(rx->id) == ident) {
                    if (rtt_ms)
                        *rtt_ms = timer_uptime_ms() - start;
                    return 0;
                }
                if (rx->type == 3)
                    return -4;             /* destination unreachable */
            }
        }
        for (volatile int i = 0; i < 3000; i++)
            ;
    }
    return -5;                             /* timeout */
}

/* ------------------------------------------------------------------ */
int net_dns_resolve(const char *name, u8 out_ip[NET_IP_LEN], u32 timeout_ms)
{
    u8 frame[NET_BUF_LEN];
    u8 query[300];
    int qlen;
    u32 start;

    if (!e1000_present() || !cfg.dns[0])
        return -1;

    memset(query, 0, sizeof(query));
    query[0] = 0x30; query[1] = 0x39;      /* id 0x3039 */
    query[2] = 0x01; query[3] = 0x00;      /* RD=1 */
    query[4] = 0; query[5] = 1;            /* 1 question */
    qlen = 12;
    {
        const char *p = name;
        while (*p) {
            const char *dot = p;
            while (*dot && *dot != '.')
                dot++;
            int lab = (int)(dot - p);
            if (lab <= 0 || lab > 63 || qlen + 1 + lab >= (int)sizeof(query) - 6)
                return -1;
            query[qlen++] = (u8)lab;
            memcpy(query + qlen, p, (u32)lab);
            qlen += lab;
            if (*dot == 0)
                break;
            p = dot + 1;
        }
        query[qlen++] = 0;                 /* root label */
    }
    /* QTYPE=A, QCLASS=IN */
    query[qlen++] = 0; query[qlen++] = 1;
    query[qlen++] = 0; query[qlen++] = 1;

    if (udp_send(cfg.dns, 4021, DNS_PORT, query, (u32)qlen) != 0)
        return -2;
    start = timer_uptime_ms();
    while (timer_uptime_ms() - start < timeout_ms) {
        int n = e1000_recv(frame, sizeof(frame));
        if (n > (int)(ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + 12)) {
            u8 *pl;
            u8 proto, src[4];
            int plen = ip_input(frame, n, &pl, &proto, src);

            if (plen > 0 && proto == PROTO_UDP && be16(((udp_hdr_t *)pl)->src_port) == DNS_PORT) {
                u8 *dns = pl + UDP_HDR_LEN;
                u16 qdcount = ((u16)dns[4] << 8) | dns[5];
                u16 ancount = ((u16)dns[6] << 8) | dns[7];
                int off = 12;

                if (ancount == 0)
                    return -3;
                /* skip the questions */
                for (int i = 0; i < qdcount; i++) {
                    while (off < plen - UDP_HDR_LEN && dns[off])
                        off += dns[off] + 1;
                    off += 5;
                }
                /* first answer: name (maybe compressed), type, class, ttl, rdlen */
                if (off + 12 > plen - UDP_HDR_LEN)
                    return -4;
                if (dns[off] & 0xC0)
                    off += 2;
                else
                    while (off < plen - UDP_HDR_LEN && dns[off])
                        off += dns[off] + 1;
                off += 8;                        /* type(2) class(2) ttl(4) */
                if (off + 2 > plen - UDP_HDR_LEN)
                    return -4;
                {
                    int rdlen = (dns[off] << 8) | dns[off + 1];
                    off += 2;
                    if (rdlen == 4 && off + 4 <= plen - UDP_HDR_LEN) {
                        memcpy(out_ip, dns + off, 4);
                        return 0;
                    }
                    return -5;
                }
            }
        }
        for (volatile int i = 0; i < 3000; i++)
            ;
    }
    return -6;
}

/* ------------------------------------------------------------------ */
void net_init(void)
{
    if (!e1000_present())
        return;
    e1000_mac(my_mac);
    memset(&cfg, 0, sizeof(cfg));
    memset(arp_cache, 0, sizeof(arp_cache));
    kprintf("[net] stack ready, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
            my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
}

const net_cfg_t *net_cfg(void)
{
    return &cfg;
}

int net_link_up(void)
{
    return e1000_present() && have_ip;
}

/* ------------------------------------------------------------------ */
void net_ip_to_str(const u8 ip[4], char *buf, int buflen)
{
    snprintf(buf, (u32)buflen, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
}

int net_str_to_ip(const char *s, u8 out[4])
{
    u32 v[4] = { 0, 0, 0, 0 };
    int part = 0, digits = 0;

    for (; *s; s++) {
        if (*s == '.') {
            if (++part > 3 || digits == 0 || v[part - 1] > 255)
                return -1;
            digits = 0;
        } else if (*s >= '0' && *s <= '9') {
            v[part] = v[part] * 10 + (u32)(*s - '0');
            if (v[part] > 255)
                return -1;
            digits++;
        } else {
            return -1;
        }
    }
    if (part != 3 || digits == 0)
        return -1;
    out[0] = (u8)v[0];
    out[1] = (u8)v[1];
    out[2] = (u8)v[2];
    out[3] = (u8)v[3];
    return 0;
}
