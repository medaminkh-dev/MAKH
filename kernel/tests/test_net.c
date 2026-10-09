/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_net.c
 * Brutal tests for the Phase 14 network stack.
 *
 * Loopback tests are fully deterministic and exercise the whole stack
 * (Ethernet framing, IPv4, ICMP, UDP, TCP, sockets). The lossy-loopback tests
 * drop every Nth frame to force retransmission paths. NIC tests talk to QEMU's
 * user-mode network (gateway 10.0.2.2) through the e1000 driver.
 */

#include <ktest.h>
#include <net/net.h>
#include <net/socket.h>
#include <drivers/e1000.h>
#include <arch/idt.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <ktime.h>
#include <kernel.h>
#include <klog.h>
#include <mm/kheap.h>
#include <lib/string.h>

extern int tcp_active_count(void);

static struct sockaddr_in addr_of(uint32_t ip, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}
#define SA(a) ((struct sockaddr*)&(a))

static void set_timeout(int fd, int opt, uint64_t ms) {
    struct timeval tv = { (int64_t)(ms / 1000), (int64_t)((ms % 1000) * 1000) };
    setsockopt(fd, SOL_SOCKET, opt, &tv, sizeof(tv));
}

/* Wait (sleeping) until every TCP control block has been reaped. */
static int wait_tcbs_drained(int baseline, uint64_t timeout_ms) {
    uint64_t end = clock_now_ms() + timeout_ms;
    for (;;) {
        net_lock();
        int n = tcp_active_count();
        net_unlock();
        if (n <= baseline) return 1;
        if (clock_now_ms() >= end) return 0;
        sched_sleep_ms(20);
    }
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

KTEST(net, inet_checksum_vectors) {
    /* RFC 1071 section 3 example: words 0001 f203 f4f5 f6f7 -> sum ddf2,
     * checksum ~ddf2 = 220d. */
    const uint8_t v[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7 };
    KEXPECT_EQ(ntohs(inet_checksum(v, sizeof(v))), 0x220d);

    /* A buffer that includes its own checksum verifies to zero. */
    uint8_t w[10];
    memcpy(w, v, 8);
    uint16_t c = inet_checksum(v, 8);
    memcpy(w + 8, &c, 2);
    KEXPECT_EQ(inet_checksum(w, 10), 0);

    /* Odd length pads with a zero byte. */
    const uint8_t odd[] = { 0xAB };
    KEXPECT_EQ(ntohs(inet_checksum(odd, 1)), (uint16_t)~0xAB00);
}

KTEST(net, ip_string_roundtrip) {
    char b[16];
    uint32_t ip = 0;
    KEXPECT_EQ(ip_from_str("10.0.2.15", &ip), 0);
    KEXPECT_EQ(ip, IPV4(10, 0, 2, 15));
    KEXPECT_EQ(strcmp(ip_to_str(ip, b), "10.0.2.15"), 0);
    KEXPECT_EQ(strcmp(ip_to_str(IPV4(255, 255, 255, 0), b), "255.255.255.0"), 0);
    KEXPECT_EQ(strcmp(ip_to_str(0, b), "0.0.0.0"), 0);
    /* Garbage must be rejected, never half-parsed. */
    KEXPECT_NE(ip_from_str("256.1.1.1", &ip), 0);
    KEXPECT_NE(ip_from_str("1.2.3", &ip), 0);
    KEXPECT_NE(ip_from_str("1.2.3.4.5", &ip), 0);
    KEXPECT_NE(ip_from_str("a.b.c.d", &ip), 0);
    KEXPECT_NE(ip_from_str("", &ip), 0);
    KEXPECT_NE(ip_from_str("1..2.3", &ip), 0);
}

KTEST(net, route_selection) {
    uint32_t hop = 0;
    netdev_t* d = ipv4_route(IPV4(127, 0, 0, 1), &hop);
    KASSERT_TEST(d != NULL);
    KEXPECT(d->is_loopback);
    if (e1000_present()) {
        d = ipv4_route(IPV4(10, 0, 2, 2), &hop);        /* on-link */
        KASSERT_TEST(d != NULL);
        KEXPECT_EQ(hop, IPV4(10, 0, 2, 2));
        d = ipv4_route(IPV4(8, 8, 8, 8), &hop);         /* via gateway */
        KASSERT_TEST(d != NULL);
        KEXPECT_EQ(hop, IPV4(10, 0, 2, 2));
        d = ipv4_route(IPV4(10, 0, 2, 15), &hop);       /* ourselves -> lo */
        KASSERT_TEST(d != NULL);
        KEXPECT(d->is_loopback);
    }
}

/* -------------------------------------------------------------------------- */
/* ICMP over loopback                                                         */
/* -------------------------------------------------------------------------- */

KTEST(net, ping_loopback) {
    for (uint16_t seq = 1; seq <= 5; seq++) {
        net_lock();
        int rtt = icmp_ping(IPV4(127, 0, 0, 1), seq, 1000, 0);
        net_unlock();
        KEXPECT(rtt >= 0);
    }
}

/* -------------------------------------------------------------------------- */
/* UDP over loopback                                                          */
/* -------------------------------------------------------------------------- */

KTEST(net, udp_loopback_roundtrip) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    int cli = socket(AF_INET, SOCK_DGRAM, 0);
    KASSERT_TEST(srv >= 0 && cli >= 0);

    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 7000);
    KEXPECT_EQ(bind(srv, SA(sa), sizeof(sa)), 0);
    set_timeout(srv, SO_RCVTIMEO, 1000);
    set_timeout(cli, SO_RCVTIMEO, 1000);

    const char msg[] = "makh-udp-hello";
    KEXPECT_EQ(sendto(cli, msg, sizeof(msg), 0, SA(sa), sizeof(sa)), (long)sizeof(msg));

    char buf[64];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    long n = recvfrom(srv, buf, sizeof(buf), 0, SA(from), &flen);
    KEXPECT_EQ(n, (long)sizeof(msg));
    KEXPECT_EQ(memcmp(buf, msg, sizeof(msg)), 0);
    KEXPECT_EQ(ntohl(from.sin_addr.s_addr), INADDR_LOOPBACK);

    /* Reply to the sender's ephemeral port. */
    KEXPECT_EQ(sendto(srv, "pong", 4, 0, SA(from), flen), 4);
    n = recv(cli, buf, sizeof(buf), 0);
    KEXPECT_EQ(n, 4);
    KEXPECT_EQ(memcmp(buf, "pong", 4), 0);

    sock_close(srv);
    sock_close(cli);
}

KTEST(net, udp_many_datagrams_in_order) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    int cli = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 7001);
    KEXPECT_EQ(bind(srv, SA(sa), sizeof(sa)), 0);
    set_timeout(srv, SO_RCVTIMEO, 1000);
    KEXPECT_EQ(connect(cli, SA(sa), sizeof(sa)), 0);   /* UDP default destination */

    enum { N = 40 };
    for (int i = 0; i < N; i++) {
        uint32_t v = 0xC0DE0000u | (uint32_t)i;
        KEXPECT_EQ(send(cli, &v, sizeof(v), 0), 4);
        if ((i & 7) == 7) sched_sleep_ms(10);   /* let netd drain */
    }
    for (int i = 0; i < N; i++) {
        uint32_t v = 0;
        KEXPECT_EQ(recv(srv, &v, sizeof(v), 0), 4);
        KEXPECT_EQ(v, 0xC0DE0000u | (uint32_t)i);    /* loopback preserves order */
    }
    sock_close(srv);
    sock_close(cli);
}

KTEST(net, udp_timeout_and_truncation) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 7002);
    bind(s, SA(sa), sizeof(sa));
    set_timeout(s, SO_RCVTIMEO, 50);

    char b[8];
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(recv(s, b, sizeof(b), 0), -1);
    KEXPECT_EQ(errno, EAGAIN);
    KEXPECT(clock_now_ms() - t0 >= 40);
    KEXPECT_EQ(recv(s, b, sizeof(b), MSG_DONTWAIT), -1);
    KEXPECT_EQ(errno, EAGAIN);

    /* A datagram larger than the buffer is truncated, not overflowed. */
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    char big[100];
    memset(big, 'Z', sizeof(big));
    sendto(c, big, sizeof(big), 0, SA(sa), sizeof(sa));
    char small[8];
    memset(small, 0, sizeof(small));
    set_timeout(s, SO_RCVTIMEO, 1000);
    KEXPECT_EQ(recv(s, small, 4, 0), 4);
    KEXPECT_EQ(small[4], 0);                     /* nothing written past len */
    sock_close(c);
    sock_close(s);
}

/* -------------------------------------------------------------------------- */
/* Socket API error paths                                                     */
/* -------------------------------------------------------------------------- */

KTEST(net, socket_api_errors) {
    KEXPECT_EQ(socket(99, SOCK_STREAM, 0), -1);
    KEXPECT_EQ(errno, EAFNOSUPPORT);
    KEXPECT_EQ(socket(AF_INET, 99, 0), -1);
    KEXPECT_EQ(errno, EPROTONOSUPPORT);
    KEXPECT_EQ(sock_close(-1), -1);
    KEXPECT_EQ(errno, EBADF);
    KEXPECT_EQ(send(12345, "x", 1, 0), -1);
    KEXPECT_EQ(errno, EBADF);

    int a = socket(AF_INET, SOCK_DGRAM, 0);
    int b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 7003);
    KEXPECT_EQ(bind(a, SA(sa), sizeof(sa)), 0);
    KEXPECT_EQ(bind(b, SA(sa), sizeof(sa)), -1);        /* port taken */
    KEXPECT_EQ(errno, EADDRINUSE);
    KEXPECT_EQ(bind(a, SA(sa), sizeof(sa)), -1);        /* already bound */
    KEXPECT_EQ(errno, EINVAL);
    KEXPECT_EQ(send(b, "x", 1, 0), -1);                 /* no destination */
    KEXPECT_EQ(errno, EDESTADDRREQ);
    KEXPECT_EQ(listen(a, 1), -1);                       /* UDP can't listen */
    KEXPECT_EQ(errno, EOPNOTSUPP);

    struct sockaddr_in bad = sa;
    bad.sin_family = 99;
    KEXPECT_EQ(connect(b, SA(bad), sizeof(bad)), -1);
    KEXPECT_EQ(errno, EAFNOSUPPORT);
    KEXPECT_EQ(bind(b, SA(sa), 3), -1);                 /* short sockaddr */
    KEXPECT_EQ(errno, EINVAL);
    sock_close(a);
    sock_close(b);
}

/* -------------------------------------------------------------------------- */
/* TCP over loopback                                                          */
/* -------------------------------------------------------------------------- */

/* Echo server: accept one client, echo until EOF, return total bytes. */
static void* tcp_echo_server(void* arg) {
    int lfd = (int)(long)arg;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) return (void*)(long)-1;
    char buf[700];
    long total = 0;
    for (;;) {
        long n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            long w = send(c, buf + off, (size_t)(n - off), 0);
            if (w <= 0) { sock_close(c); return (void*)(long)-2; }
            off += w;
        }
        total += n;
    }
    sock_close(c);
    return (void*)total;
}

static int make_listener(uint16_t port, int backlog) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0) return -1;
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = addr_of(INADDR_ANY, port);
    if (bind(l, SA(sa), sizeof(sa)) < 0 || listen(l, backlog) < 0) {
        sock_close(l);
        return -1;
    }
    set_timeout(l, SO_RCVTIMEO, 5000);
    return l;
}

KTEST(net, tcp_loopback_echo) {
    net_lock();
    int base_tcbs = tcp_active_count();
    net_unlock();
    int base_socks = sock_open_count();

    int l = make_listener(8001, 4);
    KASSERT_TEST(l >= 0);
    pthread_t srv;
    pthread_create(&srv, NULL, tcp_echo_server, (void*)(long)l);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    set_timeout(c, SO_RCVTIMEO, 3000);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 8001);
    KASSERT_TEST(connect(c, SA(sa), sizeof(sa)) == 0);

    const char msg[] = "hello from the MAKH TCP stack";
    KEXPECT_EQ(send(c, msg, sizeof(msg), 0), (long)sizeof(msg));
    char got[64];
    long have = 0;
    while (have < (long)sizeof(msg)) {
        long n = recv(c, got + have, sizeof(got) - (size_t)have, 0);
        if (n <= 0) break;
        have += n;
    }
    KEXPECT_EQ(have, (long)sizeof(msg));
    KEXPECT_EQ(memcmp(got, msg, sizeof(msg)), 0);

    /* Half-close: server sees EOF, closes, we see EOF. */
    KEXPECT_EQ(shutdown(c, SHUT_WR), 0);
    KEXPECT_EQ(recv(c, got, sizeof(got), 0), 0);
    sock_close(c);

    void* total = NULL;
    pthread_join(srv, &total);
    KEXPECT_EQ((long)total, (long)sizeof(msg));
    sock_close(l);

    /* Every connection (incl. TIME_WAIT) must be reaped: no leaks. The wait
     * returns as soon as the TCBs drain, so a generous bound only adds
     * tolerance when the reap timer is starved under heavy parallel-stress
     * load (24x4 QEMU on a contended host); a genuine leak never drains and
     * still fails. */
    KEXPECT(wait_tcbs_drained(base_tcbs, 10000));
    KEXPECT_EQ(sock_open_count(), base_socks);
}

/* Sink server: count bytes and a position-dependent checksum until EOF. */
typedef struct sink_result { long bytes; uint64_t sum; } sink_result_t;
static sink_result_t sink_res;

static void* tcp_sink_server(void* arg) {
    int lfd = (int)(long)arg;
    int c = accept(lfd, NULL, NULL);
    if (c < 0) { sink_res.bytes = -1; return NULL; }
    set_timeout(c, SO_RCVTIMEO, 15000);
    uint8_t buf[1024];
    long pos = 0;
    uint64_t sum = 0;
    for (;;) {
        long n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (long i = 0; i < n; i++) {
            sum += (uint64_t)buf[i] * (uint64_t)((pos + i) % 251 + 1);
        }
        pos += n;
    }
    sink_res.bytes = pos;
    sink_res.sum = sum;
    sock_close(c);
    return NULL;
}

static uint8_t pattern_byte(long i) { return (uint8_t)((i * 31 + 7) ^ (i >> 7)); }

static int tcp_bulk_transfer(uint16_t port, long total, unsigned drop_one_in) {
    int l = make_listener(port, 2);
    if (l < 0) return -1;
    sink_res.bytes = 0;
    sink_res.sum = 0;
    pthread_t srv;
    pthread_create(&srv, NULL, tcp_sink_server, (void*)(long)l);

    net_loopback_set_drop(drop_one_in);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    set_timeout(c, SO_SNDTIMEO, 15000);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, port);
    int rc = connect(c, SA(sa), sizeof(sa));

    uint64_t expect = 0;
    long sent = 0;
    uint8_t chunk[900];
    while (rc == 0 && sent < total) {
        long n = total - sent < (long)sizeof(chunk) ? total - sent : (long)sizeof(chunk);
        for (long i = 0; i < n; i++) {
            chunk[i] = pattern_byte(sent + i);
            expect += (uint64_t)chunk[i] * (uint64_t)((sent + i) % 251 + 1);
        }
        long w = send(c, chunk, (size_t)n, 0);
        if (w <= 0) { rc = -2; break; }
        /* Account for a short write by only advancing what was accepted. */
        if (w < n) {
            for (long i = w; i < n; i++)
                expect -= (uint64_t)chunk[i] * (uint64_t)((sent + i) % 251 + 1);
        }
        sent += w;
    }
    shutdown(c, SHUT_WR);
    pthread_join(srv, NULL);
    sock_close(c);
    sock_close(l);
    net_loopback_set_drop(0);

    if (rc != 0) return rc;
    if (sink_res.bytes != total) return -3;
    if (sink_res.sum != expect) return -4;
    return 0;
}

KTEST(net, tcp_bulk_200k_flow_control) {
    /* 200 KB through a 16 KB window: exercises buffering, window updates and
     * blocking send; every byte is verified with a position-weighted sum. */
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(tcp_bulk_transfer(8002, 200 * 1024, 0), 0);
    kprintf("    200 KB loopback transfer in %lu ms\n", (unsigned long)(clock_now_ms() - t0));
}

KTEST(net, tcp_bulk_with_packet_loss) {
    /* Drop every 7th frame (data, ACKs, even SYNs): fast retransmit and the
     * RTO must still deliver every byte exactly once and in order. */
    uint64_t retrans_before = net_stats.tcp_retrans;
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(tcp_bulk_transfer(8003, 64 * 1024, 7), 0);
    uint64_t retrans = net_stats.tcp_retrans - retrans_before;
    kprintf("    64 KB with 1/7 loss in %lu ms, %lu retransmissions\n",
            (unsigned long)(clock_now_ms() - t0), (unsigned long)retrans);
    KEXPECT(retrans > 0);                        /* loss really happened */
}

KTEST(net, tcp_connect_refused) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 8999);   /* nobody listening */
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(connect(c, SA(sa), sizeof(sa)), -1);
    KEXPECT_EQ(errno, ECONNREFUSED);
    KEXPECT(clock_now_ms() - t0 < 1000);         /* RST, not a timeout */
    sock_close(c);
}

/* Several clients against one server thread-per-connection. */
static void* conn_worker(void* arg) {
    int fd = (int)(long)arg;
    char b[128];
    long total = 0, n;
    while ((n = recv(fd, b, sizeof(b), 0)) > 0) {
        send(fd, b, (size_t)n, 0);
        total += n;
    }
    sock_close(fd);
    return (void*)total;
}

KTEST(net, tcp_concurrent_clients) {
    enum { N = 4 };
    size_t heap0 = kheap_get_used();
    int l = make_listener(8004, N);
    KASSERT_TEST(l >= 0);

    int cli[N];
    struct sockaddr_in sa = addr_of(INADDR_LOOPBACK, 8004);
    for (int i = 0; i < N; i++) {
        cli[i] = socket(AF_INET, SOCK_STREAM, 0);
        set_timeout(cli[i], SO_RCVTIMEO, 3000);
        KEXPECT_EQ(connect(cli[i], SA(sa), sizeof(sa)), 0);
    }
    pthread_t w[N];
    for (int i = 0; i < N; i++) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int fd = accept(l, SA(peer), &pl);
        KASSERT_TEST(fd >= 0);
        KEXPECT_EQ(ntohl(peer.sin_addr.s_addr), INADDR_LOOPBACK);
        pthread_create(&w[i], NULL, conn_worker, (void*)(long)fd);
    }
    for (int i = 0; i < N; i++) {
        char msg[16], got[16];
        for (int k = 0; k < 16; k++) msg[k] = (char)('A' + i + k);
        KEXPECT_EQ(send(cli[i], msg, 16, 0), 16);
        long have = 0;
        while (have < 16) {
            long n = recv(cli[i], got + have, (size_t)(16 - have), 0);
            if (n <= 0) break;
            have += n;
        }
        KEXPECT_EQ(have, 16);
        KEXPECT_EQ(memcmp(msg, got, 16), 0);    /* no cross-talk between conns */
        shutdown(cli[i], SHUT_WR);
    }
    for (int i = 0; i < N; i++) {
        void* r;
        pthread_join(w[i], &r);
        KEXPECT_EQ((long)r, 16);
        sock_close(cli[i]);
    }
    sock_close(l);

    /* Workers joined, sockets closed: only the clients' TIME_WAIT tcbs remain
     * and after a timer tick they must have released their 32 KB buffers. */
    sched_sleep_ms(150);
    KEXPECT(kheap_get_used() < heap0 + 8 * 1024);
    KEXPECT_EQ(kheap_check(), 0);
}

static void* blocked_acceptor(void* arg) {
    int l = (int)(long)arg;
    int fd = accept(l, NULL, NULL);
    return (void*)(long)(fd == -1 ? errno : 0);
}

KTEST(net, tcp_accept_timeout_and_close_unblocks) {
    int l = make_listener(8005, 1);
    KASSERT_TEST(l >= 0);
    set_timeout(l, SO_RCVTIMEO, 60);
    uint64_t t0 = clock_now_ms();
    KEXPECT_EQ(accept(l, NULL, NULL), -1);
    KEXPECT_EQ(errno, EAGAIN);
    KEXPECT(clock_now_ms() - t0 >= 50);
    sock_close(l);

    /* A thread blocked in accept() with no timeout must be woken (EBADF) when
     * another thread closes the listener, not left sleeping forever. */
    l = make_listener(8006, 1);
    KASSERT_TEST(l >= 0);
    pthread_t t;
    KASSERT_TEST(pthread_create(&t, NULL, blocked_acceptor, (void*)(long)l) == 0);
    sched_sleep_ms(50);                          /* let it block */
    sock_close(l);
    void* r;
    KEXPECT_EQ(pthread_join(t, &r), 0);
    KEXPECT_EQ((long)r, EBADF);
}

/* -------------------------------------------------------------------------- */
/* Real NIC (e1000) against QEMU user networking                              */
/* -------------------------------------------------------------------------- */

KTEST(net_hw, arp_resolves_gateway) {
    if (!e1000_present()) { kprintf("    (no e1000: skipped)\n"); return; }
    netdev_t* eth = netdev_by_name("eth0");
    KASSERT_TEST(eth != NULL);
    uint8_t mac[ETH_ALEN];
    net_lock();
    int rc = arp_resolve(eth, IPV4(10, 0, 2, 2), mac, 2000);
    net_unlock();
    KEXPECT_EQ(rc, 0);
    /* QEMU's slirp gateway uses the 52:55:0a:00:02:02 address. */
    KEXPECT_EQ(mac[0], 0x52);
    KEXPECT_EQ(mac[1], 0x55);
    kprintf("    gateway 10.0.2.2 is at %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

KTEST(net_hw, ping_gateway) {
    if (!e1000_present()) { kprintf("    (no e1000: skipped)\n"); return; }
    int ok = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        net_lock();
        int rtt = icmp_ping(IPV4(10, 0, 2, 2), seq, 1000, 0);
        net_unlock();
        if (rtt >= 0) ok++;
    }
    kprintf("    %d/4 echo replies from 10.0.2.2\n", ok);
    KEXPECT(ok >= 3);
}

KTEST(net_hw, nic_counters_move) {
    /* Self-contained: generate our own traffic and check the deltas, so the
     * result doesn't depend on which test happened to run first. */
    if (!e1000_present()) { kprintf("    (no e1000: skipped)\n"); return; }
    netdev_t* eth = netdev_by_name("eth0");
    KASSERT_TEST(eth != NULL);
    uint64_t tx0 = eth->tx_packets, rx0 = eth->rx_packets;
    uint64_t irq0 = irq_get_count(e1000_irq_line());

    net_lock();
    int rtt = icmp_ping(IPV4(10, 0, 2, 2), 77, 1000, 0);
    net_unlock();
    KEXPECT(rtt >= 0);

    KEXPECT(eth->tx_packets > tx0);
    KEXPECT(eth->rx_packets > rx0);
    KEXPECT(irq_get_count(e1000_irq_line()) > irq0);   /* RX really raised an IRQ */
}
