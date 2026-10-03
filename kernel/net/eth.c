/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - net/eth.c
 * Ethernet framing: build outgoing frames and dispatch incoming ones.
 * All callers hold net_lock, which also protects the shared TX scratch buffer.
 */

#include <net/net.h>
#include <lib/string.h>

static const uint8_t BROADCAST[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* One frame-sized scratch buffer; drivers copy out of it before returning. */
static uint8_t tx_frame[ETH_FRAME_MAX];

int eth_send(netdev_t* dev, const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
             const void* payload, size_t len) {
    if (!dev || !dev->send || len > ETH_MTU) return -1;

    eth_hdr_t* eh = (eth_hdr_t*)tx_frame;
    memcpy(eh->dst, dst_mac ? dst_mac : BROADCAST, ETH_ALEN);
    memcpy(eh->src, dev->mac, ETH_ALEN);
    eh->type = htons(ethertype);
    memcpy(tx_frame + ETH_HLEN, payload, len);

    size_t flen = ETH_HLEN + len;
    if (flen < ETH_ZLEN) {                       /* pad runt frames */
        memset(tx_frame + flen, 0, ETH_ZLEN - flen);
        flen = ETH_ZLEN;
    }
    return dev->send(dev, tx_frame, flen);
}

void net_input(netdev_t* dev, const uint8_t* frame, size_t len) {
    if (len < ETH_HLEN) { dev->rx_dropped++; return; }

    const eth_hdr_t* eh = (const eth_hdr_t*)frame;

    /* Accept frames addressed to us or broadcast (the NIC filters too, but a
     * fuzzer or loopback may hand us anything). */
    if (!dev->is_loopback &&
        memcmp(eh->dst, dev->mac, ETH_ALEN) != 0 &&
        memcmp(eh->dst, BROADCAST, ETH_ALEN) != 0) {
        return;
    }

    const uint8_t* payload = frame + ETH_HLEN;
    size_t plen = len - ETH_HLEN;

    switch (ntohs(eh->type)) {
        case ETHERTYPE_ARP:
            arp_input(dev, payload, plen);
            break;
        case ETHERTYPE_IPV4:
            ipv4_input(dev, payload, plen);
            break;
        default:
            break;  /* unknown ethertype: ignore */
    }
}

/* Source MAC of the frame currently being processed (for ARP gleaning). */
const uint8_t* eth_rx_src_mac(const uint8_t* ip_payload) {
    return ip_payload - ETH_HLEN + ETH_ALEN;
}
