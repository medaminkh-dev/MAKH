/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Copy-on-write: the child mutating a shared page must not change the parent's. */
#include "usys.h"

/* A writable global lands in .data, which fork marks copy-on-write. */
volatile int g = 111;

int umain(void) {
    long pid = ufork();
    if (pid < 0) return 1;
    if (pid == 0) {
        g = 222;                       /* child's write takes a private copy */
        return 0;
    }
    int st = -1;
    uwaitpid((int)pid, &st);
    if (g != 111) return 2;            /* parent's copy must be untouched */
    return 0;
}
