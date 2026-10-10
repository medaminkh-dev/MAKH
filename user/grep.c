/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* grep <pattern> [file...]: print lines containing the (literal) pattern. */
#include "usys.h"
static int contains(const char* line,int len,const char* pat){
    int pl=0; while(pat[pl])pl++;
    if(!pl)return 1;
    for(int i=0;i+pl<=len;i++){int j=0;while(j<pl&&line[i+j]==pat[j])j++;if(j==pl)return 1;}
    return 0;
}
static int grep_fd(int fd,const char* pat){
    char buf[1024]; static char line[2048]; int ll=0; long n; int found=0;
    while((n=uread(fd,buf,sizeof buf))>0){
        for(long i=0;i<n;i++){
            char c=buf[i];
            if(c=='\n'){ if(contains(line,ll,pat)){uwrite(1,line,(unsigned long)ll);uwrite(1,"\n",1);found=1;} ll=0; }
            else if(ll<(int)sizeof(line)-1) line[ll++]=c;
        }
    }
    if(ll>0&&contains(line,ll,pat)){uwrite(1,line,(unsigned long)ll);uwrite(1,"\n",1);found=1;}
    return found;
}
int umain(int argc,char**argv){
    if(argc<2){uwrite(2,"usage: grep <pattern> [file...]\n",32);return 2;}
    int found=0;
    if(argc==2) found=grep_fd(0,argv[1]);
    else for(int i=2;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;if(grep_fd(fd,argv[1]))found=1;uclose(fd);}
    return found?0:1;
}
