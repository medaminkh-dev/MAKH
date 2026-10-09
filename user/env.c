/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* env: print the environment, one NAME=VALUE per line. */
#include "usys.h"
static unsigned long sl(const char* s){unsigned long n=0;while(s[n])n++;return n;}
int umain(int argc, char** argv){
    /* On the SysV initial stack envp follows argv's NULL terminator. */
    char** envp = argv + argc + 1;
    for(char** e=envp; *e; e++){ uwrite(1,*e,sl(*e)); uwrite(1,"\n",1); }
    return 0;
}
