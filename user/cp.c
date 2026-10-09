/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* cp: copy src to dst. If dst is a directory, copy to dst/<basename(src)>. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
static const char* base(const char* p){const char* b=p;for(const char* s=p;*s;s++)if(*s=='/')b=s+1;return b;}
int umain(int argc, char** argv){
    if(argc<3){uwrite(2,"usage: cp <src> <dst>\n",22);return 1;}
    char dstbuf[256];
    const char* dst=argv[2];
    struct stat st;
    if(ustat(dst,&st)==0 && S_ISDIR(st.st_mode)){
        int p=0; const char* d=dst; while(*d)dstbuf[p++]=*d++;
        if(p&&dstbuf[p-1]!='/')dstbuf[p++]='/';
        const char* b=base(argv[1]); while(*b)dstbuf[p++]=*b++; dstbuf[p]=0;
        dst=dstbuf;
    }
    long in=uopen(argv[1],O_RDONLY);
    if(in<0){uwrite(2,"cp: ",4);uwrite(2,argv[1],sl(argv[1]));uwrite(2,": cannot open\n",14);return 1;}
    long out=uopen(dst,O_WRONLY|O_CREAT|O_TRUNC);
    if(out<0){uclose((int)in);uwrite(2,"cp: cannot create dst\n",22);return 1;}
    char buf[1024]; long n; int rc=0;
    while((n=uread((int)in,buf,sizeof buf))>0){
        long off=0; while(off<n){long w=uwrite((int)out,buf+off,(unsigned long)(n-off)); if(w<=0){rc=1;break;} off+=w;}
        if(rc)break;
    }
    uclose((int)in); uclose((int)out);
    return rc;
}
