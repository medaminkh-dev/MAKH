/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * curl URL — fetch an HTTP URL and print the response body to stdout.
 * MAKH's own minimal client: HTTP/1.0 over the ring-3 socket layer, with DNS, so
 * the host may be a name or a dotted-quad IP. The scheme and port are optional:
 *   curl example.com
 *   curl http://example.com/
 *   curl https://example.com/      (no TLS: served as plain HTTP on port 80)
 *   curl 10.0.2.2:8000/index.html
 * Use -i to include the response headers.
 */
#include "usys.h"
#include "uresolve.h"

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void eput(const char* s){ uwrite(2, s, slen(s)); }

/* Split URL into host/port/path. The scheme is optional and handled internally:
 * "yahoo.com", "http://yahoo.com", "https://yahoo.com/x", "host:8080/p" all work
 * (any "scheme://" prefix is skipped; there is no TLS, so https talks plain HTTP).
 * The port defaults to 80. Returns 0 or -1. */
static int parse_url(const char* url, char* host, int hostsz, unsigned short* port, const char** path){
    const char* p = url;
    for (const char* q = url; *q; q++) {            /* skip an optional "scheme://" */
        if (q[0]==':' && q[1]=='/' && q[2]=='/') { p = q + 3; break; }
        if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||(*q>='0'&&*q<='9')||*q=='+'||*q=='-'||*q=='.')) break;
    }
    const char* hs = p;
    while (*p && *p != ':' && *p != '/') p++;
    int hl = (int)(p - hs);
    if (hl <= 0 || hl >= hostsz) return -1;
    for (int i=0;i<hl;i++) host[i] = hs[i];
    host[hl] = 0;
    unsigned int pv = 80;
    if (*p == ':') { p++; pv = 0; while (*p>='0'&&*p<='9') pv = pv*10 + (*p++ - '0'); if (!pv) pv = 80; }
    *path = (*p == '/') ? p : "/";
    *port = (unsigned short)pv;
    return 0;
}

/* HTTP GET; write the response to fd `out`. Returns 0 or -1. */
static int http_get(const char* url, int out, int show_hdr){
    char host[128]; unsigned short port; const char* path;
    if (parse_url(url, host, sizeof host, &port, &path) < 0) { eput("curl: bad URL\n"); return -1; }
    unsigned int ip;
    if (uresolve(host, &ip) < 0) { eput("curl: cannot resolve "); eput(host); eput("\n"); return -1; }

    long fd = usocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { eput("curl: socket failed\n"); return -1; }
    struct sockaddr_in sa; for (unsigned i=0;i<sizeof sa;i++) ((char*)&sa)[i]=0;
    sa.sin_family = AF_INET; sa.sin_port = uhtons(port); sa.sin_addr.s_addr = uhtonl(ip);
    if (uconnect((int)fd, &sa, sizeof sa) < 0) { eput("curl: connect failed\n"); uclose((int)fd); return -1; }

    char req[640]; int n = 0;
    for (const char* s="GET ";*s;s++) req[n++]=*s;
    for (const char* s=path;*s;s++) req[n++]=*s;
    for (const char* s=" HTTP/1.0\r\nHost: ";*s;s++) req[n++]=*s;
    for (const char* s=host;*s;s++) req[n++]=*s;
    for (const char* s="\r\nConnection: close\r\n\r\n";*s;s++) req[n++]=*s;
    uwrite((int)fd, req, (unsigned long)n);

    char buf[1024]; long r; int past = show_hdr;
    static char acc[4096]; int al=0, found=0;
    while ((r = uread((int)fd, buf, sizeof buf)) > 0) {
        if (past) { uwrite(out, buf, (unsigned long)r); continue; }
        for (long i=0;i<r;i++){
            if (al < (int)sizeof acc) acc[al++] = buf[i];
            if (!found && al>=4 && acc[al-4]=='\r'&&acc[al-3]=='\n'&&acc[al-2]=='\r'&&acc[al-1]=='\n'){
                found=1; past=1; long rest=r-1-i;
                if (rest>0) uwrite(out, buf+i+1, (unsigned long)rest);
                break;
            }
        }
    }
    uclose((int)fd);
    if (!found && !show_hdr && al>0) uwrite(out, acc, (unsigned long)al);
    return 0;
}

int umain(int argc, char** argv){
    int show_hdr=0; const char* url=0;
    for (int i=1;i<argc;i++){
        if (argv[i][0]=='-' && argv[i][1]=='i') show_hdr=1;
        else url=argv[i];
    }
    if (!url) { eput("usage: curl [-i] [http[s]://]host[:port][/path]\n"); return 2; }
    return http_get(url, 1, show_hdr) < 0 ? 1 : 0;
}
