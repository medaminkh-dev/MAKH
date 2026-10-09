/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* rmdir: remove empty directories. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
int umain(int argc, char** argv){
    if(argc<2){uwrite(2,"usage: rmdir <dir>...\n",22);return 1;}
    int rc=0;
    for(int i=1;i<argc;i++)
        if(urmdir(argv[i])<0){uwrite(2,"rmdir: ",7);uwrite(2,argv[i],sl(argv[i]));uwrite(2,": cannot remove\n",16);rc=1;}
    return rc;
}
