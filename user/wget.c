/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * wget URL [-O file] — download an HTTP URL to a file (MAKH's own, HTTP/1.0,
 * no TLS/DNS, so the host is a dotted-quad IP):
 *   wget http://10.0.2.2:8000/data.bin
 *   wget -O out.html http://10.0.2.2:8000/
 * With no -O the output name is the last path component, or index.html.
 */
#include "usys.h"

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void eput(const char* s){ uwrite(2, s, slen(s)); }

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

static int parse_url(const char* url, unsigned int* ip, unsigned short* port, const char** path){
    const char* p = url; const char* sch="http://";
    int pre=1; for(int i=0;i<7;i++) if(p[i]!=sch[i]){ pre=0; break; }
    if(pre) p+=7;
    const char* hs=p; while(*p && *p!=':' && *p!='/') p++;
    const char* he=p; unsigned int pv=80;
    if(*p==':'){ p++; pv=0; while(*p>='0'&&*p<='9') pv=pv*10+(*p++ -'0'); }
    *path = (*p=='/') ? p : "/";
    if(parse_ip(hs,he,ip)<0) return -1;
    *port=(unsigned short)pv; return 0;
}

/* pick an output name from the path's last component */
static const char* basename_of(const char* path){
    const char* b = path;
    for(const char* p=path; *p; p++) if(*p=='/') b=p+1;
    return (*b) ? b : "index.html";
}

int umain(int argc, char** argv){
    const char* url=0; const char* outname=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='O'){ if(i+1<argc) outname=argv[++i]; }
        else url=argv[i];
    }
    if(!url){ eput("usage: wget [-O file] http://IP[:port]/path\n"); return 2; }

    unsigned int ip; unsigned short port; const char* path;
    if(parse_url(url,&ip,&port,&path)<0){ eput("wget: bad URL (use an IP)\n"); return 1; }
    if(!outname) outname = basename_of(path);

    long fd = usocket(AF_INET, SOCK_STREAM, 0);
    if(fd<0){ eput("wget: socket failed\n"); return 1; }
    struct sockaddr_in sa; for(unsigned i=0;i<sizeof sa;i++) ((char*)&sa)[i]=0;
    sa.sin_family=AF_INET; sa.sin_port=uhtons(port); sa.sin_addr.s_addr=uhtonl(ip);
    if(uconnect((int)fd,&sa,sizeof sa)<0){ eput("wget: connect failed\n"); uclose((int)fd); return 1; }

    char req[512]; int n=0;
    for(const char* s="GET ";*s;s++) req[n++]=*s;
    for(const char* s=path;*s;s++) req[n++]=*s;
    for(const char* s=" HTTP/1.0\r\nHost: makh\r\nConnection: close\r\n\r\n";*s;s++) req[n++]=*s;
    uwrite((int)fd, req, (unsigned long)n);

    int out = (int)uopen(outname, O_WRONLY|O_CREAT|O_TRUNC);
    if(out<0){ eput("wget: cannot create "); eput(outname); eput("\n"); uclose((int)fd); return 1; }

    char buf[1024]; long r; int past=0, found=0; unsigned long total=0;
    static char acc[4096]; int al=0;
    while((r=uread((int)fd,buf,sizeof buf))>0){
        if(past){ uwrite(out,buf,(unsigned long)r); total+=(unsigned long)r; continue; }
        for(long i=0;i<r;i++){
            if(al<(int)sizeof acc) acc[al++]=buf[i];
            if(!found && al>=4 && acc[al-4]=='\r'&&acc[al-3]=='\n'&&acc[al-2]=='\r'&&acc[al-1]=='\n'){
                found=1; past=1; long rest=r-1-i;
                if(rest>0){ uwrite(out,buf+i+1,(unsigned long)rest); total+=(unsigned long)rest; }
                break;
            }
        }
    }
    if(!found && al>0){ uwrite(out,acc,(unsigned long)al); total+=(unsigned long)al; }
    uclose(out); uclose((int)fd);

    eput("wget: saved "); eput(outname); eput("\n");
    (void)total;
    return 0;
}
