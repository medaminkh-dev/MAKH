/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* mkdir: create directories. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
int umain(int argc, char** argv){
    if(argc<2){uwrite(2,"usage: mkdir <dir>...\n",22);return 1;}
    int rc=0;
    for(int i=1;i<argc;i++)
        if(umkdir(argv[i])<0){uwrite(2,"mkdir: ",7);uwrite(2,argv[i],sl(argv[i]));uwrite(2,": cannot create\n",16);rc=1;}
    return rc;
}
