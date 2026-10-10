/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* touch: create each file if it does not exist. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
int umain(int argc, char** argv){
    if(argc<2){uwrite(2,"usage: touch <file>...\n",23);return 1;}
    int rc=0;
    for(int i=1;i<argc;i++){
        long fd=uopen(argv[i],O_WRONLY|O_CREAT);
        if(fd<0){uwrite(2,"touch: ",7);uwrite(2,argv[i],sl(argv[i]));uwrite(2,": cannot create\n",16);rc=1;}
        else uclose((int)fd);
    }
    return rc;
}
