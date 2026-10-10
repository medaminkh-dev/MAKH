/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* rm: remove files (and empty dirs). No recursive -r yet. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
int umain(int argc, char** argv){
    if(argc<2){uwrite(2,"usage: rm <file>...\n",20);return 1;}
    int rc=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-') continue;                 /* ignore flags (e.g. -f) */
        if(uunlink(argv[i])<0){uwrite(2,"rm: ",4);uwrite(2,argv[i],sl(argv[i]));uwrite(2,": cannot remove\n",16);rc=1;}
    }
    return rc;
}
