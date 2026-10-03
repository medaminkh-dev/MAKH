/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - net/arp.c
 * Address Resolution Protocol: IPv4 -> Ethernet MAC cache, requests, replies.
 *
 * Deadlock rule: the netd thread processes incoming ARP replies, so netd must
 * never block waiting for one. arp_resolve() therefore only waits when called
 * from an application thread; from netd it sends a request and returns -EAGAIN
 * (the caller drops the packet and TCP/ICMP retries recover). Sources of
 * packets we receive are "gleaned" into the cache, so replies from netd to a
 * peer that just talked to us are almost always resolvable immediately.
 */

#include <net/net.h>
#include <pthread.h>
#include <ktime.h>
#include <errno.h>
#include <lib/string.h>
#include <klog.h>

#define ARP_CACHE_SIZE   32
#define ARP_TTL_MS       (60 * 1000)
#define ARP_RETRY_MS     300

#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint64_t expires;
    int      valid;
} arp_entry_t;

static arp_entry_t cache[ARP_CACHE_SIZE];
static pthread_cond_t arp_cond = PTHREAD_COND_INITIALIZER;

extern int net_in_netd(void);

/* ---------------------------------------------------------------- cache */

static arp_entry_t* find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (cache[i].valid && cache[i].ip == ip) return &cache[i];
    return NULL;
}

void arp_update(uint32_t ip, const uint8_t mac[ETH_ALEN]) {
    if (ip == 0) return;
    arp_entry_t* e = find(ip);
    if (!e) {
        /* Take a free slot, else evict the entry closest to expiry. */
        e = &cache[0];
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!cache[i].valid) { e = &cache[i]; break; }
            if (cache[i].expires < e->expires) e = &cache[i];
        }
    }
    e->ip = ip;
    memcpy(e->mac, mac, ETH_ALEN);
    e->expires = clock_now_ms() + ARP_TTL_MS;
    e->valid = 1;
    pthread_cond_broadcast(&arp_cond);
}

int arp_lookup(uint32_t ip, uint8_t mac_out[ETH_ALEN]) {
    arp_entry_t* e = find(ip);
    if (!e) return -1;
    memcpy(mac_out, e->mac, ETH_ALEN);
    return 0;
}

int arp_cache_count(void) {
    int n = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) n += cache[i].valid;
    return n;
}

/* Print the cache (the shell's `arp`). Caller holds net_lock. */
void arp_cache_dump(void) {
    uint64_t now = clock_now_ms();
    char ip[16];
    kprintf("Address          HWaddress           Expires(s)\n");
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!cache[i].valid) continue;
        const uint8_t* m = cache[i].mac;
        kprintf("%-16s %02x:%02x:%02x:%02x:%02x:%02x   %lu\n", ip_to_str(cache[i].ip, ip),
                m[0], m[1], m[2], m[3], m[4], m[5],
                (unsigned long)((cache[i].expires - now) / 1000));
    }
}

void arp_timer(void) {
    uint64_t now = clock_now_ms();
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (cache[i].valid && now >= cache[i].expires) cache[i].valid = 0;
}

/* ---------------------------------------------------------------- wire */

static void arp_send(netdev_t* dev, uint16_t op, const uint8_t tha[ETH_ALEN],
                     uint32_t tpa, const uint8_t* eth_dst) {
    arp_pkt_t p;
    p.htype = htons(1);
    p.ptype = htons(ETHERTYPE_IPV4);
    p.hlen = ETH_ALEN;
    p.plen = 4;
    p.oper = htons(op);
    memcpy(p.sha, dev->mac, ETH_ALEN);
    p.spa = htonl(dev->ip);
    if (tha) memcpy(p.tha, tha, ETH_ALEN); else memset(p.tha, 0, ETH_ALEN);
    p.tpa = htonl(tpa);
    net_stats.arp_tx++;
    eth_send(dev, eth_dst, ETHERTYPE_ARP, &p, sizeof(p));
}

void arp_input(netdev_t* dev, const uint8_t* pkt, size_t len) {
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t* p = (const arp_pkt_t*)pkt;
    if (ntohs(p->htype) != 1 || ntohs(p->ptype) != ETHERTYPE_IPV4 ||
        p->hlen != ETH_ALEN || p->plen != 4) {
        return;
    }
    net_stats.arp_rx++;

    uint32_t spa = ntohl(p->spa);
    uint32_t tpa = ntohl(p->tpa);
    uint16_t op = ntohs(p->oper);

    /* Learn the sender if we already know it or the packet is for us. */
    if (spa != 0 && (find(spa) || tpa == dev->ip)) {
        arp_update(spa, p->sha);
    }

    if (op == ARP_OP_REQUEST && tpa == dev->ip && dev->ip != 0) {
        arp_send(dev, ARP_OP_REPLY, p->sha, spa, p->sha);
    }
}

/* ---------------------------------------------------------------- resolve */

int arp_resolve(netdev_t* dev, uint32_t ip, uint8_t mac_out[ETH_ALEN], uint64_t timeout_ms) {
    if (dev->is_loopback) {                /* no link layer on loopback */
        memset(mac_out, 0, ETH_ALEN);
        return 0;
    }
    if (ip == 0xFFFFFFFFu) {               /* limited broadcast */
        memset(mac_out, 0xFF, ETH_ALEN);
        return 0;
    }
    if (arp_lookup(ip, mac_out) == 0) return 0;

    arp_send(dev, ARP_OP_REQUEST, NULL, ip, NULL);
    if (timeout_ms == 0 || net_in_netd()) return -EAGAIN;

    uint64_t deadline = clock_now_ms() + timeout_ms;
    uint64_t next_retry = clock_now_ms() + ARP_RETRY_MS;
    for (;;) {
        uint64_t now = clock_now_ms();
        if (now >= deadline) return -EHOSTUNREACH;
        uint64_t slice = (next_retry < deadline ? next_retry : deadline) - now;
        net_wait(&arp_cond, slice ? slice : 1);   /* drops net_lock while waiting */
        if (arp_lookup(ip, mac_out) == 0) return 0;
        if (clock_now_ms() >= next_retry) {
            arp_send(dev, ARP_OP_REQUEST, NULL, ip, NULL);
            next_retry = clock_now_ms() + ARP_RETRY_MS;
        }
    }
}
