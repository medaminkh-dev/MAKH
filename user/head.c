/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* head [-n N] [file...]: print the first N lines (default 10). */
#include "usys.h"
static void head_fd(int fd,int maxl){
    char buf[1024]; long n; int lines=0;
    while((n=uread(fd,buf,sizeof buf))>0)
        for(long i=0;i<n;i++){uwrite(1,buf+i,1);if(buf[i]=='\n'&&++lines>=maxl)return;}
}
static int atoin(const char* s){int v=0;while(*s>='0'&&*s<='9')v=v*10+(*s++-'0');return v;}
int umain(int argc,char**argv){
    int maxl=10,fi=1;
    if(argc>=3&&argv[1][0]=='-'&&argv[1][1]=='n'){maxl=atoin(argv[2]);if(maxl<=0)maxl=10;fi=3;}
    if(fi>=argc){head_fd(0,maxl);return 0;}
    for(int i=fi;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;head_fd(fd,maxl);uclose(fd);}
    return 0;
}
