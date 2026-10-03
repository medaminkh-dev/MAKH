/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - net/udp.c
 * UDP: datagram demux to sockets, checksum validation, and output.
 */

#include "sock.h"
#include <errno.h>
#include <mm/kheap.h>
#include <lib/string.h>

extern uint32_t ipv4_src_for(uint32_t dst);

void udp_input(netdev_t* dev, uint32_t src, uint32_t dst, const uint8_t* pkt, size_t len) {
    (void)dev;
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t* h = (const udp_hdr_t*)pkt;

    size_t ulen = ntohs(h->len);
    if (ulen < sizeof(udp_hdr_t) || ulen > len) return;

    /* A zero checksum means "not computed" (legal for UDP over IPv4). */
    if (h->checksum != 0 && inet_pseudo_checksum(src, dst, IPPROTO_UDP, pkt, ulen) != 0) {
        return;
    }
    net_stats.udp_rx++;

    sock_t* s = udp_lookup(dst, ntohs(h->dst_port));
    if (!s || s->shut_rd || s->dq_len >= UDP_RXQ_MAX) return;   /* drop */

    size_t plen = ulen - sizeof(udp_hdr_t);
    dgram_t* d = kmalloc(sizeof(dgram_t) + plen);
    if (!d) return;
    d->next = NULL;
    d->src_ip = src;
    d->src_port = ntohs(h->src_port);
    d->len = plen;
    memcpy(d->data, pkt + sizeof(udp_hdr_t), plen);

    if (s->dq_tail) s->dq_tail->next = d; else s->dq_head = d;
    s->dq_tail = d;
    s->dq_len++;
    pthread_cond_broadcast(&s->cond);
}

int udp_output(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
               const void* data, size_t len) {
    size_t total = sizeof(udp_hdr_t) + len;
    uint8_t* seg = kmalloc(total);
    if (!seg) return -ENOBUFS;

    if (src_ip == 0) src_ip = ipv4_src_for(dst_ip);
    if (src_ip == 0) { kfree(seg); return -ENETUNREACH; }

    udp_hdr_t* h = (udp_hdr_t*)seg;
    h->src_port = htons(src_port);
    h->dst_port = htons(dst_port);
    h->len = htons((uint16_t)total);
    h->checksum = 0;
    memcpy(seg + sizeof(udp_hdr_t), data, len);
    uint16_t c = inet_pseudo_checksum(src_ip, dst_ip, IPPROTO_UDP, seg, total);
    h->checksum = c ? c : 0xFFFF;              /* 0 is reserved for "none" */

    net_stats.udp_tx++;
    int rc = ipv4_send(dst_ip, IPPROTO_UDP, seg, total);
    kfree(seg);
    return rc < 0 ? rc : 0;
}
