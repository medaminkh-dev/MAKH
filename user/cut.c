/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* cut -c N[-M] | -f N [-d C]: select characters or a delimited field per line. */
#include "usys.h"
static char buf[65536];
static int atoin(const char* s,const char** e){int v=0;while(*s>='0'&&*s<='9'){v=v*10+(*s-'0');s++;}if(e)*e=s;return v;}
static void do_line(const char* ln,int len,int cmode,int c1,int c2,int fnum,char delim){
    if(cmode){for(int i=c1-1;i<c2&&i<len;i++)if(i>=0)uwrite(1,ln+i,1);uwrite(1,"\n",1);}
    else{int f=1,start=0;for(int i=0;i<=len;i++){if(i==len||ln[i]==delim){if(f==fnum){uwrite(1,ln+start,(unsigned long)(i-start));uwrite(1,"\n",1);return;}f++;start=i+1;}}uwrite(1,"\n",1);}
}
static int slurp(int fd,int len){long n;while((n=uread(fd,buf+len,sizeof(buf)-1-(unsigned)len))>0){len+=(int)n;if(len>=(int)sizeof(buf)-1)break;}return len;}
int umain(int argc,char**argv){
    int cmode=0,c1=1,c2=1000000,fnum=1,fi=argc; char delim='\t';
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'&&argv[i][1]=='c'){const char* r=argv[i][2]?argv[i]+2:(++i<argc?argv[i]:"1");const char* e;c1=atoin(r,&e);c2=(*e=='-')?(e[1]?atoin(e+1,0):1000000):c1;cmode=1;}
        else if(argv[i][0]=='-'&&argv[i][1]=='f'){const char* r=argv[i][2]?argv[i]+2:(++i<argc?argv[i]:"1");fnum=atoin(r,0);}
        else if(argv[i][0]=='-'&&argv[i][1]=='d'){const char* d=argv[i][2]?argv[i]+2:(++i<argc?argv[i]:"\t");delim=d[0];}
        else {fi=i;break;}
    }
    int len=(fi<argc)?0:slurp(0,0);
    if(fi<argc){for(int i=fi;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;len=slurp(fd,len);uclose(fd);}}
    int start=0;
    for(int i=0;i<len;i++) if(buf[i]=='\n'){do_line(buf+start,i-start,cmode,c1,c2,fnum,delim);start=i+1;}
    if(start<len) do_line(buf+start,len-start,cmode,c1,c2,fnum,delim);
    return 0;
}
