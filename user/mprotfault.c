/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* mmap a page RW, drop it to read-only, then write -> must take SIGSEGV. */
#include "usys.h"

int umain(void) {
    unsigned char* p = (unsigned char*)umap(4096, PROT_READ | PROT_WRITE,
                                            MAP_ANONYMOUS | MAP_PRIVATE);
    if ((long)p <= 0) return 1;
    p[0] = 1;                                  /* writable: fine */
    if (umprotect(p, 4096, PROT_READ) != 0) return 2;
    p[0] = 2;                                  /* read-only: faults -> SIGSEGV */
    return 0;                                  /* unreachable if mprotect worked */
}
