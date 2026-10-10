/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* tail [-n N] [file...]: print the last N lines (default 10). Caps at 64 KiB. */
#include "usys.h"
static char buf[65536];
static void tail_fd(int fd,int maxl){
    int len=0; long n;
    while((n=uread(fd,buf+len,sizeof(buf)-(unsigned)len))>0){
        len+=(int)n;
        if(len>=(int)sizeof(buf)){int keep=sizeof(buf)/2;for(int i=0;i<keep;i++)buf[i]=buf[(int)sizeof(buf)-keep+i];len=keep;}
    }
    int nl=0,start=0;
    for(int i=len-1;i>=0;i--) if(buf[i]=='\n'&&++nl>maxl){start=i+1;break;}
    if(len>start)uwrite(1,buf+start,(unsigned long)(len-start));
}
static int atoin(const char* s){int v=0;while(*s>='0'&&*s<='9')v=v*10+(*s++-'0');return v;}
int umain(int argc,char**argv){
    int maxl=10,fi=1;
    if(argc>=3&&argv[1][0]=='-'&&argv[1][1]=='n'){maxl=atoin(argv[2]);if(maxl<=0)maxl=10;fi=3;}
    if(fi>=argc){tail_fd(0,maxl);return 0;}
    for(int i=fi;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;tail_fd(fd,maxl);uclose(fd);}
    return 0;
}
