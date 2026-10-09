/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_NET_H
#define MAKHOS_NET_H

#include <types.h>

/**
 * =============================================================================
 * net.h - MakhOS network stack core
 * =============================================================================
 * Architecture (single-CPU, thread-based):
 *
 *   NIC IRQ / loopback send ──> net_rx_notify() ──> [netd thread]
 *                                                     │  (holds net_lock)
 *                                                     ├─ dev->poll(): drain RX
 *                                                     │    └─ net_input(): eth → arp | ipv4
 *                                                     │                       └─ icmp | udp | tcp
 *                                                     └─ periodic timers (ARP retry, TCP RTO)
 *
 *   app threads ──> socket API (takes net_lock, blocks on pthread cond vars)
 *                    └─ ip_send() → route → arp_resolve → dev->send()
 *
 * All protocol state is protected by one global mutex, net_lock (a monitor):
 * simple, deadlock-free, and fine for a single CPU. Drivers never take it.
 * Addresses are kept in HOST byte order internally; conversion happens at the
 * wire boundary.
 * =============================================================================
 */

/* ---------------------------------------------------------------- byte order */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v >> 8) & 0xFF00) | (v >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#define IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ---------------------------------------------------------------- constants */

#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HLEN + ETH_MTU)
#define ETH_ZLEN        60          /* minimum frame length (without FCS) */

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806

#define IPPROTO_ICMP    1
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

/* ---------------------------------------------------------------- wire headers */

typedef struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;                  /* network order */
} eth_hdr_t;

typedef struct __attribute__((packed)) arp_pkt {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} arp_pkt_t;

typedef struct __attribute__((packed)) ipv4_hdr {
    uint8_t  ver_ihl;               /* version (4) << 4 | header length in words */
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;              /* flags(3) | fragment offset(13) */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ipv4_hdr_t;

typedef struct __attribute__((packed)) icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

typedef struct __attribute__((packed)) udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} udp_hdr_t;

typedef struct __attribute__((packed)) tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;              /* header length in words << 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* ---------------------------------------------------------------- devices */

typedef struct netdev {
    char     name[8];
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;                    /* host order */
    uint32_t netmask;
    uint32_t gateway;
    int      up;
    int      is_loopback;

    /* Transmit one complete Ethernet frame. Must not block for long and must
     * not take net_lock. Returns 0 on success. */
    int  (*send)(struct netdev* dev, const void* frame, size_t len);
    /* Drain received frames into net_input(). Called by netd with net_lock. */
    void (*poll)(struct netdev* dev);
    void* priv;

    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped, tx_dropped;
} netdev_t;

#define NET_MAX_DEVS 4

/* ---------------------------------------------------------------- core API */

void      net_init(void);                        /* loopback + netd thread */
int       netdev_register(netdev_t* dev);
netdev_t* netdev_get(int index);
int       netdev_count(void);
netdev_t* netdev_by_name(const char* name);
void      netdev_set_default(netdev_t* dev);     /* route for non-local traffic */
netdev_t* netdev_default(void);

/* Wake the netd thread (safe from IRQ context). */
void net_rx_notify(void);

/* Stack entry point for a received frame (netd context, net_lock held). */
void net_input(netdev_t* dev, const uint8_t* frame, size_t len);

/* Global lock protecting all protocol state. */
void net_lock(void);
void net_unlock(void);
/* Condition-wait on the net lock with an optional timeout (ms, 0 = forever).
 * Returns 0 if signalled, ETIMEDOUT otherwise. */
int  net_wait(void* cond, uint64_t timeout_ms);
void net_signal_all(void* cond);

/* Loopback fault injection (for brutal tests): drop 1-in-N frames (0 = off). */
void net_loopback_set_drop(unsigned one_in_n);

/* ---------------------------------------------------------------- helpers */

uint16_t inet_checksum(const void* data, size_t len);
/* Checksum with an IPv4 pseudo-header, for UDP/TCP (addresses host order). */
uint16_t inet_pseudo_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                              const void* data, size_t len);

/* Format "a.b.c.d" into buf (>= 16 bytes). Returns buf. */
char* ip_to_str(uint32_t ip, char* buf);
/* Parse "a.b.c.d"; returns 0 on success. */
int   ip_from_str(const char* s, uint32_t* out);

/* ---------------------------------------------------------------- protocols */

/* eth.c */
int  eth_send(netdev_t* dev, const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
              const void* payload, size_t len);

/* arp.c */
void arp_input(netdev_t* dev, const uint8_t* pkt, size_t len);
/* Resolve ip -> mac. May drop net_lock while waiting. Returns 0 or -errno. */
int  arp_resolve(netdev_t* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN], uint64_t timeout_ms);
int  arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]);   /* cache only */
void arp_timer(void);
int  arp_cache_count(void);
void arp_cache_dump(void);                                 /* needs net_lock */

/* ipv4.c */
void ipv4_input(netdev_t* dev, const uint8_t* pkt, size_t len);
/* Route + send an IPv4 packet (payload after the IP header). */
int  ipv4_send(uint32_t dst, uint8_t proto, const void* payload, size_t len);
netdev_t* ipv4_route(uint32_t dst, uint32_t* next_hop);

/* icmp.c */
void icmp_input(netdev_t* dev, uint32_t src, uint32_t dst, uint8_t ttl,
                const uint8_t* pkt, size_t len);
/* Send an echo request and wait for the reply. Returns RTT in ms, or -errno.
 * If ttl_out is non-NULL, it receives the reply packet's IP TTL. */
int  icmp_ping(uint32_t dst, uint16_t seq, uint64_t timeout_ms, uint8_t* ttl_out);

/* udp.c */
void udp_input(netdev_t* dev, uint32_t src, uint32_t dst, const uint8_t* pkt, size_t len);

/* tcp.c */
void tcp_input(netdev_t* dev, uint32_t src, uint32_t dst, const uint8_t* pkt, size_t len);
void tcp_timer(void);

/* Statistics / debug */
typedef struct net_stats {
    uint64_t ip_rx, ip_tx, ip_bad, udp_rx, udp_tx, tcp_rx, tcp_tx, tcp_retrans,
             tcp_rst_tx, icmp_rx, icmp_tx, arp_rx, arp_tx, dropped_no_route;
} net_stats_t;
extern net_stats_t net_stats;

void tcp_debug_dump(void);    /* print every TCB (for `netstat`); needs net_lock */

#endif /* MAKHOS_NET_H */
