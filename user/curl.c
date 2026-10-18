/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * curl URL — fetch an HTTP URL and print the response body to stdout.
 * MAKH's own minimal client: HTTP/1.0 over the ring-3 socket layer. No TLS and
 * no DNS yet, so the host must be a dotted-quad IP, e.g.
 *   curl http://10.0.2.2:8000/index.html
 * Use -i to include the response headers.
 */
#include "usys.h"

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void eput(const char* s){ uwrite(2, s, slen(s)); }

/* parse "A.B.C.D" into a host-order u32; returns 0 or -1 */
static int parse_ip(const char* s, const char* end, unsigned int* out){
    unsigned int v=0; int oct=0, ndig=0, cur=0;
    for(const char* p=s;; p++){
        if(p!=end && *p>='0' && *p<='9'){ cur=cur*10+(*p-'0'); if(cur>255) return -1; ndig=1; }
        else if(p==end || *p=='.'){ if(!ndig||oct>=4) return -1; v=(v<<8)|(unsigned)cur; oct++; cur=0; ndig=0; if(p==end) break; }
        else return -1;
    }
    if (oct != 4) return -1;
    *out = v;
    return 0;
}

/* Split URL into ip/port/path. Returns 0 or -1. */
static int parse_url(const char* url, unsigned int* ip, unsigned short* port, const char** path){
    const char* p = url;
    const char* sch = "http://";
    int pre = 1; for (int i=0;i<7;i++) if (p[i] != sch[i]) { pre = 0; break; }
    if (pre) p += 7;                                       /* skip scheme */
    const char* hs = p;
    while (*p && *p != ':' && *p != '/') p++;
    const char* he = p;
    unsigned int port_v = 80;
    if (*p == ':') { p++; port_v = 0; while (*p>='0'&&*p<='9') port_v = port_v*10 + (*p++ - '0'); }
    *path = (*p == '/') ? p : "/";
    if (parse_ip(hs, he, ip) < 0) return -1;
    *port = (unsigned short)port_v;
    return 0;
}

/* HTTP GET; write the response to fd `out`. If show_hdr, include headers.
 * Returns 0 on success, -1 on a network error. */
int http_get(const char* url, int out, int show_hdr){
    unsigned int ip; unsigned short port; const char* path;
    if (parse_url(url, &ip, &port, &path) < 0) { eput("bad URL (use an IP, e.g. http://10.0.2.2:8000/)\n"); return -1; }

    long fd = usocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { eput("socket failed\n"); return -1; }
    struct sockaddr_in sa; for (unsigned i=0;i<sizeof sa;i++) ((char*)&sa)[i]=0;
    sa.sin_family = AF_INET; sa.sin_port = uhtons(port); sa.sin_addr.s_addr = uhtonl(ip);
    if (uconnect((int)fd, &sa, sizeof sa) < 0) { eput("connect failed\n"); uclose((int)fd); return -1; }

    char req[512]; int n=0;
    const char* a="GET "; for(const char* s=a;*s;s++) req[n++]=*s;
    for(const char* s=path;*s;s++) req[n++]=*s;
    const char* b=" HTTP/1.0\r\nHost: makh\r\nConnection: close\r\n\r\n";
    for(const char* s=b;*s;s++) req[n++]=*s;
    uwrite((int)fd, req, (unsigned long)n);

    char buf[1024]; long r; int past_hdr = show_hdr;   /* if showing headers, pass through */
    static char acc[4096]; int al=0, found=0;
    while ((r = uread((int)fd, buf, sizeof buf)) > 0) {
        if (past_hdr) { uwrite(out, buf, (unsigned long)r); continue; }
        /* accumulate until we see the blank line that ends the headers */
        for (long i=0;i<r;i++){
            if (al < (int)sizeof acc) acc[al++] = buf[i];
            if (!found && al>=4 && acc[al-4]=='\r'&&acc[al-3]=='\n'&&acc[al-2]=='\r'&&acc[al-1]=='\n'){
                found=1; past_hdr=1;
                long rest = r-1-i;
                if (rest>0) uwrite(out, buf+i+1, (unsigned long)rest);
                break;
            }
        }
    }
    uclose((int)fd);
    if (!found && !show_hdr && al>0) uwrite(out, acc, (unsigned long)al);   /* no header break: dump all */
    return 0;
}

int umain(int argc, char** argv){
    int show_hdr=0; const char* url=0;
    for (int i=1;i<argc;i++){
        if (argv[i][0]=='-' && argv[i][1]=='i') show_hdr=1;
        else url=argv[i];
    }
    if (!url) { eput("usage: curl [-i] http://IP[:port]/path\n"); return 2; }
    return http_get(url, 1, show_hdr) < 0 ? 1 : 0;
}
