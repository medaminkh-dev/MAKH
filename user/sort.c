/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* sort: read lines (files or stdin), sort ascending, print. Caps at 64 KiB. */
#include "usys.h"
static char buf[65536];
static char* lines[4096];
static int cmp(const char* a,const char* b){while(*a&&*a==*b){a++;b++;}return (int)(unsigned char)*a-(int)(unsigned char)*b;}
static int slurp(int fd,int len){long n;while((n=uread(fd,buf+len,sizeof(buf)-1-(unsigned)len))>0){len+=(int)n;if(len>=(int)sizeof(buf)-1)break;}return len;}
int umain(int argc,char**argv){
    int len=0;
    if(argc<2) len=slurp(0,0);
    else for(int i=1;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;len=slurp(fd,len);uclose(fd);}
    buf[len]=0;
    int nl=0,start=0;
    for(int i=0;i<len;i++) if(buf[i]=='\n'){buf[i]=0;if(nl<4096)lines[nl++]=&buf[start];start=i+1;}
    if(start<len&&nl<4096)lines[nl++]=&buf[start];
    for(int i=1;i<nl;i++){char* k=lines[i];int j=i-1;while(j>=0&&cmp(lines[j],k)>0){lines[j+1]=lines[j];j--;}lines[j+1]=k;}
    for(int i=0;i<nl;i++){unsigned long L=0;while(lines[i][L])L++;uwrite(1,lines[i],L);uwrite(1,"\n",1);}
    return 0;
}
