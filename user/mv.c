/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* mv: rename a file or directory. */
#include "usys.h"
int umain(int argc, char** argv){
    if(argc<3){uwrite(2,"usage: mv <src> <dst>\n",22);return 1;}
    if(urename(argv[1],argv[2])<0){uwrite(2,"mv: cannot move\n",16);return 1;}
    return 0;
}
