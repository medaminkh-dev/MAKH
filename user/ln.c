/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* ln -s <target> <link>: create a symbolic link. (Hard links aren't supported.) */
#include "usys.h"
int umain(int argc, char** argv){
    const char* a[3]={0,0,0}; int n=0, sym=0;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'){ for(char* f=argv[i]+1;*f;f++) if(*f=='s')sym=1; }
        else if(n<2) a[n++]=argv[i];
    }
    if(n<2){uwrite(2,"usage: ln -s <target> <link>\n",29);return 1;}
    if(!sym){uwrite(2,"ln: only symbolic links (-s) are supported\n",43);return 1;}
    if(usymlink(a[0],a[1])<0){uwrite(2,"ln: cannot create link\n",23);return 1;}
    return 0;
}
