/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - net/icmp.c
 * ICMP echo: answer pings, and send pings with RTT measurement.
 */

#include <net/net.h>
#include <pthread.h>
#include <ktime.h>
#include <errno.h>
#include <mm/kheap.h>
#include <lib/string.h>

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8
#define PING_ID            0x4D41   /* "MA" */
#define PING_SLOTS         8
#define PING_PAYLOAD       32

typedef struct ping_slot {
    int      used;
    uint32_t dst;
    uint16_t seq;
    int      done;
    uint64_t sent_ms;
    uint64_t rtt_ms;
    uint8_t  ttl;
} ping_slot_t;

static ping_slot_t pings[PING_SLOTS];
static pthread_cond_t ping_cond = PTHREAD_COND_INITIALIZER;

void icmp_input(netdev_t* dev, uint32_t src, uint32_t dst, uint8_t ttl,
                const uint8_t* pkt, size_t len) {
    (void)dev; (void)dst;
    if (len < sizeof(icmp_hdr_t)) return;
    if (inet_checksum(pkt, len) != 0) return;
    net_stats.icmp_rx++;

    const icmp_hdr_t* h = (const icmp_hdr_t*)pkt;

    if (h->type == ICMP_ECHO_REQUEST && h->code == 0) {
        /* Echo the whole message back with type 0. */
        uint8_t* reply = kmalloc(len);
        if (!reply) return;
        memcpy(reply, pkt, len);
        icmp_hdr_t* r = (icmp_hdr_t*)reply;
        r->type = ICMP_ECHO_REPLY;
        r->checksum = 0;
        r->checksum = inet_checksum(reply, len);
        net_stats.icmp_tx++;
        ipv4_send(src, IPPROTO_ICMP, reply, len);
        kfree(reply);
        return;
    }

    if (h->type == ICMP_ECHO_REPLY && ntohs(h->id) == PING_ID) {
        uint16_t seq = ntohs(h->seq);
        for (int i = 0; i < PING_SLOTS; i++) {
            if (pings[i].used && !pings[i].done && pings[i].seq == seq &&
                pings[i].dst == src) {
                pings[i].rtt_ms = clock_now_ms() - pings[i].sent_ms;
                pings[i].ttl = ttl;
                pings[i].done = 1;
                pthread_cond_broadcast(&ping_cond);
                break;
            }
        }
    }
}

/* Call with net_lock held. Returns RTT in ms (>= 0) or a negative errno. */
int icmp_ping(uint32_t dst, uint16_t seq, uint64_t timeout_ms, uint8_t* ttl_out) {
    ping_slot_t* slot = NULL;
    for (int i = 0; i < PING_SLOTS; i++) {
        if (!pings[i].used) { slot = &pings[i]; break; }
    }
    if (!slot) return -ENOBUFS;
    slot->used = 1;
    slot->done = 0;
    slot->dst = dst;
    slot->seq = seq;

    uint8_t msg[sizeof(icmp_hdr_t) + PING_PAYLOAD];
    icmp_hdr_t* h = (icmp_hdr_t*)msg;
    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->id = htons(PING_ID);
    h->seq = htons(seq);
    for (int i = 0; i < PING_PAYLOAD; i++) msg[sizeof(icmp_hdr_t) + i] = (uint8_t)('a' + i % 26);
    h->checksum = 0;
    h->checksum = inet_checksum(msg, sizeof(msg));

    slot->sent_ms = clock_now_ms();
    net_stats.icmp_tx++;
    int rc = ipv4_send(dst, IPPROTO_ICMP, msg, sizeof(msg));
    if (rc < 0) { slot->used = 0; return rc; }

    uint64_t deadline = clock_now_ms() + timeout_ms;
    while (!slot->done) {
        uint64_t now = clock_now_ms();
        if (now >= deadline) break;
        net_wait(&ping_cond, deadline - now);
    }

    int result = slot->done ? (int)slot->rtt_ms : -ETIMEDOUT;
    if (slot->done && ttl_out) *ttl_out = slot->ttl;
    slot->used = 0;
    return result;
}
