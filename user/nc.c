/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * nc HOST [PORT] — a tiny netcat: open a TCP connection and relay bytes both ways
 * (stdin -> socket, socket -> stdout) using a fork, so it is full-duplex. HOST
 * may be a name or a dotted-quad (DNS via uresolve); PORT defaults to 80. E.g.
 *   echo hi | nc example.com 80
 *   nc example.com            (port 80)
 */
#include "usys.h"
#include "uresolve.h"

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void eput(const char* s){ uwrite(2, s, slen(s)); }

int umain(int argc, char** argv){
    if (argc < 2) { eput("usage: nc HOST [PORT]\n"); return 2; }
    unsigned int ip;
    if (uresolve(argv[1], &ip) < 0) { eput("nc: cannot resolve "); eput(argv[1]); eput("\n"); return 2; }
    unsigned int port = 80;                           /* default port */
    if (argc >= 3) {
        port = 0;
        for (char* p = argv[2]; *p; p++) { if (*p<'0'||*p>'9') { eput("nc: bad port\n"); return 2; } port = port*10 + (unsigned)(*p-'0'); }
        if (!port || port > 65535) { eput("nc: bad port\n"); return 2; }
    }

    long fd = usocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { eput("nc: socket failed\n"); return 1; }
    struct sockaddr_in sa; for (unsigned i=0;i<sizeof sa;i++) ((char*)&sa)[i]=0;
    sa.sin_family = AF_INET; sa.sin_port = uhtons((unsigned short)port); sa.sin_addr.s_addr = uhtonl(ip);
    if (uconnect((int)fd, &sa, sizeof sa) < 0) { eput("nc: connect failed\n"); uclose((int)fd); return 1; }

    long pid = ufork();
    if (pid == 0) {                               /* child: stdin -> socket */
        char b[1024]; long r;
        while ((r = uread(0, b, sizeof b)) > 0) uwrite((int)fd, b, (unsigned long)r);
        usyscall(SYS_EXIT, 0, 0, 0);
    }
    char b[1024]; long r;                          /* parent: socket -> stdout */
    while ((r = uread((int)fd, b, sizeof b)) > 0) uwrite(1, b, (unsigned long)r);
    uclose((int)fd);
    return 0;
}
