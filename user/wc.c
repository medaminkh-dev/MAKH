/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* wc [file...]: print line, word and byte counts (stdin if no file). */
#include "usys.h"
static int putu(char* b,unsigned long v){char t[20];int n=0;if(!v){b[0]='0';return 1;}while(v){t[n++]=(char)('0'+v%10);v/=10;}for(int i=0;i<n;i++)b[i]=t[n-1-i];return n;}
static void wc_fd(int fd,const char* name){
    char buf[1024]; long n; unsigned long L=0,W=0,B=0; int inw=0;
    while((n=uread(fd,buf,sizeof buf))>0){
        B+=(unsigned long)n;
        for(long i=0;i<n;i++){char c=buf[i];if(c=='\n')L++;if(c==' '||c=='\t'||c=='\n'||c=='\r'){inw=0;}else if(!inw){inw=1;W++;}}
    }
    char o[128];int p=0;
    p+=putu(o+p,L);o[p++]=' ';p+=putu(o+p,W);o[p++]=' ';p+=putu(o+p,B);
    if(name){o[p++]=' ';const char* s=name;while(*s)o[p++]=*s++;}
    o[p++]='\n';uwrite(1,o,(unsigned long)p);
}
int umain(int argc,char**argv){
    if(argc<2){wc_fd(0,0);return 0;}
    for(int i=1;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;wc_fd(fd,argv[i]);uclose(fd);}
    return 0;
}
