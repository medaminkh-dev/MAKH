/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * ping: send ICMP echo requests and summarise the replies. There is no socket
 * layer yet, so each echo goes through the MAKH-specific SYS_MAKH_PING call,
 * which returns (ttl<<16)|rtt_ms on a reply or a negative errno on timeout.
 *   ping <ip> [count]
 */
#include "usys.h"

static int putu(char* b, unsigned long v) {
    char t[20];
    int n = 0;
    if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}
static int puts_(char* b, const char* s) {
    int n = 0;
    while (s[n]) { b[n] = s[n]; n++; }
    return n;
}
static int emit_ip(char* b, unsigned int ip) {
    int p = 0;
    for (int i = 3; i >= 0; i--) {
        p += putu(b + p, (ip >> (i * 8)) & 0xFF);
        if (i) b[p++] = '.';
    }
    return p;
}
/* Parse a dotted quad into a host-order u32. Returns 0, or -1 on malformed. */
static int parse_ip(const char* s, unsigned int* out) {
    unsigned int v = 0;
    int oct = 0, ndig = 0, cur = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10 + (*s - '0');
            if (cur > 255) return -1;
            ndig = 1;
        } else if (*s == '.' || *s == 0) {
            if (!ndig || oct >= 4) return -1;
            v = (v << 8) | (unsigned)cur;
            oct++;
            cur = 0; ndig = 0;
            if (*s == 0) break;
        } else {
            return -1;
        }
    }
    if (oct != 4) return -1;
    *out = v;
    return 0;
}

int umain(int argc, char** argv) {
    if (argc < 2) { uwrite(2, "usage: ping <ip> [count]\n", 25); return 2; }
    unsigned int dst;
    if (parse_ip(argv[1], &dst) < 0) { uwrite(2, "ping: bad address\n", 18); return 2; }
    unsigned int count = 4;
    if (argc >= 3) {
        count = 0;
        for (char* s = argv[2]; *s; s++) {
            if (*s < '0' || *s > '9') { count = 4; break; }
            count = count * 10 + (unsigned)(*s - '0');
        }
        if (count == 0 || count > 100) count = 4;
    }

    char b[128];
    int p = puts_(b, "PING ");
    p += emit_ip(b + p, dst);
    p += puts_(b + p, ": ");
    p += putu(b + p, count);
    p += puts_(b + p, " echo requests, 56 data bytes\n");
    uwrite(1, b, (unsigned long)p);

    unsigned int ok = 0;
    unsigned long sum = 0;
    long mn = -1, mx = 0;
    for (unsigned int seq = 1; seq <= count; seq++) {
        long r = umakh_ping(dst, seq, 1000);
        p = 0;
        if (r >= 0) {
            long rtt = r & 0xFFFF, ttl = (r >> 16) & 0xFF;
            ok++;
            sum += (unsigned long)rtt;
            if (mn < 0 || rtt < mn) mn = rtt;
            if (rtt > mx) mx = rtt;
            p += puts_(b + p, "64 bytes from ");
            p += emit_ip(b + p, dst);
            p += puts_(b + p, ": icmp_seq=");
            p += putu(b + p, seq);
            p += puts_(b + p, " ttl=");
            p += putu(b + p, (unsigned long)ttl);
            p += puts_(b + p, " time=");
            p += putu(b + p, (unsigned long)rtt);
            p += puts_(b + p, " ms\n");
        } else {
            p += puts_(b + p, "icmp_seq=");
            p += putu(b + p, seq);
            p += puts_(b + p, ": no reply (timeout)\n");
        }
        uwrite(1, b, (unsigned long)p);
        if (seq < count) usleep_ms(500);
    }

    p = puts_(b, "--- ");
    p += emit_ip(b + p, dst);
    p += puts_(b + p, " ping statistics ---\n");
    uwrite(1, b, (unsigned long)p);
    p = 0;
    p += putu(b + p, count);
    p += puts_(b + p, " packets transmitted, ");
    p += putu(b + p, ok);
    p += puts_(b + p, " received, ");
    p += putu(b + p, (count - ok) * 100 / count);
    p += puts_(b + p, "% packet loss\n");
    uwrite(1, b, (unsigned long)p);
    if (ok) {
        p = puts_(b, "rtt min/avg/max = ");
        p += putu(b + p, (unsigned long)mn);
        b[p++] = '/';
        p += putu(b + p, sum / ok);
        b[p++] = '/';
        p += putu(b + p, (unsigned long)mx);
        p += puts_(b + p, " ms\n");
        uwrite(1, b, (unsigned long)p);
    }
    return ok ? 0 : 1;
}
