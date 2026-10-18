/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * uresolve.h — a tiny DNS resolver for MAKH user programs, so curl/wget/nc can
 * take a hostname (yahoo.com) and not just a dotted-quad IP. It sends one A-record
 * query over UDP (the ring-3 socket layer: a connected SOCK_DGRAM with a recv
 * timeout) and parses the first A record from the answer.
 *
 * The nameserver is read from /etc/resolv.conf ("nameserver X.X.X.X") if present,
 * otherwise it defaults to 10.0.2.3 — QEMU slirp's built-in resolver. No caching,
 * no search domains, no IPv6: just enough to turn a name into an address.
 */
#ifndef MAKH_URESOLVE_H
#define MAKH_URESOLVE_H
#include "usys.h"

#define UR_DEFAULT_NS 0x0A000203u   /* 10.0.2.3, host order */
#define UR_DNS_PORT   53

static int ur_parse_ip(const char* s, unsigned int* out) {
    unsigned int v = 0; int oct = 0, ndig = 0, cur = 0;
    for (const char* p = s;; p++) {
        if (*p >= '0' && *p <= '9') { cur = cur*10 + (*p-'0'); if (cur > 255) return -1; ndig = 1; }
        else if (*p == '.' || *p == 0) { if (!ndig || oct >= 4) return -1; v = (v<<8)|(unsigned)cur; oct++; cur = 0; ndig = 0; if (*p == 0) break; }
        else return -1;
    }
    if (oct != 4) return -1;
    *out = v;
    return 0;
}

/* Read "nameserver X.X.X.X" (optionally "...:PORT") from /etc/resolv.conf; fall
 * back to the slirp default. Sets *port (default 53). Returns the server IP. */
static unsigned int ur_nameserver(unsigned short* port) {
    *port = UR_DNS_PORT;
    int fd = (int)uopen("/etc/resolv.conf", O_RDONLY);
    if (fd < 0) return UR_DEFAULT_NS;
    char b[256]; long n = uread(fd, b, sizeof b - 1); uclose(fd);
    if (n <= 0) return UR_DEFAULT_NS;
    b[n] = 0;
    const char* key = "nameserver";
    for (long i = 0; i + 10 < n; i++) {
        int m = 1; for (int j = 0; j < 10; j++) if (b[i+j] != key[j]) { m = 0; break; }
        if (!m) continue;
        long k = i + 10; while (b[k] == ' ' || b[k] == '\t') k++;
        long e = k; while (b[e] && b[e] != '\n' && b[e] != ' ' && b[e] != '\r') e++;
        char save = b[e]; b[e] = 0;
        /* split an optional :PORT */
        char* colon = 0;
        for (char* p = b + k; *p; p++) if (*p == ':') { colon = p; break; }
        unsigned short pv = UR_DNS_PORT;
        if (colon) { *colon = 0; unsigned int t = 0; for (char* p = colon + 1; *p >= '0' && *p <= '9'; p++) t = t*10 + (*p-'0'); if (t) pv = (unsigned short)t; }
        unsigned int ip; int ok = ur_parse_ip(b + k, &ip);
        b[e] = save;
        if (ok == 0) { *port = pv; return ip; }
    }
    return UR_DEFAULT_NS;
}

/* Encode "www.ex.com" as DNS labels; returns length incl. the trailing 0, or -1. */
static int ur_qname(const char* host, unsigned char* buf) {
    int bi = 0; const char* p = host;
    while (*p) {
        const char* s = p; int len = 0;
        while (*p && *p != '.') { p++; len++; }
        if (len == 0 || len > 63) return -1;
        buf[bi++] = (unsigned char)len;
        for (int i = 0; i < len; i++) buf[bi++] = (unsigned char)s[i];
        if (*p == '.') p++;
    }
    buf[bi++] = 0;
    return bi;
}

/* Skip a DNS name (labels or a compression pointer) starting at off. */
static int ur_skip_name(const unsigned char* r, int off, long rn) {
    while (off < rn) {
        unsigned char len = r[off];
        if (len == 0) return off + 1;
        if ((len & 0xC0) == 0xC0) return off + 2;
        off += len + 1;
    }
    return off;
}

/* Resolve host -> ip (host order). Accepts a dotted-quad directly. 0 / -1. */
static int uresolve(const char* host, unsigned int* ip_out) {
    if (ur_parse_ip(host, ip_out) == 0) return 0;

    unsigned short nsport;
    unsigned int ns = ur_nameserver(&nsport);
    long fd = usocket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct timeval tv; tv.tv_sec = 4; tv.tv_usec = 0;
    usetsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, &tv);
    struct sockaddr_in sa; for (unsigned i = 0; i < sizeof sa; i++) ((char*)&sa)[i] = 0;
    sa.sin_family = AF_INET; sa.sin_port = uhtons(nsport); sa.sin_addr.s_addr = uhtonl(ns);
    if (uconnect((int)fd, &sa, sizeof sa) < 0) { uclose((int)fd); return -1; }

    unsigned char q[300]; int qi = 0;
    q[qi++] = 0x4d; q[qi++] = 0x4b;      /* id "MK"               */
    q[qi++] = 0x01; q[qi++] = 0x00;      /* flags: recursion des. */
    q[qi++] = 0;    q[qi++] = 1;         /* QDCOUNT = 1           */
    q[qi++] = 0;    q[qi++] = 0;         /* ANCOUNT               */
    q[qi++] = 0;    q[qi++] = 0;         /* NSCOUNT               */
    q[qi++] = 0;    q[qi++] = 0;         /* ARCOUNT               */
    int nl = ur_qname(host, q + qi); if (nl < 0) { uclose((int)fd); return -1; } qi += nl;
    q[qi++] = 0; q[qi++] = 1;            /* QTYPE  = A            */
    q[qi++] = 0; q[qi++] = 1;            /* QCLASS = IN           */
    if (uwrite((int)fd, q, (unsigned long)qi) < 0) { uclose((int)fd); return -1; }

    unsigned char r[512];
    long rn = uread((int)fd, r, sizeof r);
    uclose((int)fd);
    if (rn < 12) return -1;
    int ancount = (r[6] << 8) | r[7];
    if (ancount < 1) return -1;

    int off = ur_skip_name(r, 12, rn) + 4;     /* question: name + QTYPE + QCLASS */
    for (int a = 0; a < ancount && off + 10 <= rn; a++) {
        off = ur_skip_name(r, off, rn);
        if (off + 10 > rn) break;
        int type  = (r[off] << 8) | r[off+1]; off += 2;
        off += 2;                               /* class */
        off += 4;                               /* ttl   */
        int rdlen = (r[off] << 8) | r[off+1]; off += 2;
        if (type == 1 && rdlen == 4 && off + 4 <= rn) {
            *ip_out = ((unsigned)r[off]<<24)|((unsigned)r[off+1]<<16)|((unsigned)r[off+2]<<8)|r[off+3];
            return 0;
        }
        off += rdlen;
    }
    return -1;
}
#endif /* MAKH_URESOLVE_H */
