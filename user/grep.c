/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* grep <pattern> [file...]: print lines containing the (literal) pattern.
 * When stdout is a terminal, the matched text is highlighted in bright red
 * (like an interactive grep); piped or redirected, output stays plain. */
#include "usys.h"

static int patlen(const char* p){ int n=0; while(p[n])n++; return n; }

static int contains(const char* line,int len,const char* pat,int pl){
    if(!pl)return 1;
    for(int i=0;i+pl<=len;i++){int j=0;while(j<pl&&line[i+j]==pat[j])j++;if(j==pl)return 1;}
    return 0;
}

/* Print one matching line. With colour on, wrap each (non-overlapping) match in
 * bright-red SGR, flushing the plain runs between matches in one write each. */
static void emit_line(const char* line,int len,const char* pat,int pl,int color){
    if(!color||!pl){ uwrite(1,line,(unsigned long)len); uwrite(1,"\n",1); return; }
    int i=0, run=0;
    while(i<len){
        int m=0;
        if(i+pl<=len){ int j=0; while(j<pl&&line[i+j]==pat[j])j++; m=(j==pl); }
        if(m){
            if(i>run) uwrite(1,line+run,(unsigned long)(i-run));
            uwrite(1,"\033[1;31m",7); uwrite(1,line+i,(unsigned long)pl); uwrite(1,"\033[0m",4);
            i+=pl; run=i;
        } else i++;
    }
    if(len>run) uwrite(1,line+run,(unsigned long)(len-run));
    uwrite(1,"\n",1);
}

static int grep_fd(int fd,const char* pat,int pl,int color){
    char buf[1024]; static char line[2048]; int ll=0; long n; int found=0;
    while((n=uread(fd,buf,sizeof buf))>0){
        for(long i=0;i<n;i++){
            char c=buf[i];
            if(c=='\n'){ if(contains(line,ll,pat,pl)){emit_line(line,ll,pat,pl,color);found=1;} ll=0; }
            else if(ll<(int)sizeof(line)-1) line[ll++]=c;
        }
    }
    if(ll>0&&contains(line,ll,pat,pl)){emit_line(line,ll,pat,pl,color);found=1;}
    return found;
}

int umain(int argc,char**argv){
    if(argc<2){uwrite(2,"usage: grep <pattern> [file...]\n",32);return 2;}
    const char* pat=argv[1]; int pl=patlen(pat);
    struct termios t; int color=(ugettermios(1,&t)>=0);   /* colour only to a tty */
    int found=0;
    if(argc==2) found=grep_fd(0,pat,pl,color);
    else for(int i=2;i<argc;i++){int fd=(int)uopen(argv[i],O_RDONLY);if(fd<0)continue;if(grep_fd(fd,pat,pl,color))found=1;uclose(fd);}
    return found?0:1;
}
