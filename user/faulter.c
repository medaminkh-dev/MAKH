/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include "usys.h"
int umain(void) {
    volatile long* bad = (volatile long*)0x1234;   /* unmapped -> #PF -> SIGSEGV */
    *bad = 1;
    return 0;                       /* not reached */
}
