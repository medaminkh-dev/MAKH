/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise the anonymous-memory syscalls end to end; return 0 iff all hold. */
#include "usys.h"

int umain(void) {
    unsigned long len = 2 * 4096;

    /* mmap a region, stamp a pattern, read it back, then release it. */
    unsigned char* p = (unsigned char*)umap(len, PROT_READ | PROT_WRITE,
                                            MAP_ANONYMOUS | MAP_PRIVATE);
    if ((long)p <= 0) return 1;
    for (unsigned long i = 0; i < len; i++) p[i] = (unsigned char)(i * 7 + 3);
    for (unsigned long i = 0; i < len; i++)
        if (p[i] != (unsigned char)(i * 7 + 3)) return 2;
    if (umunmap(p, len) != 0) return 3;

    /* brk: query, grow one page, use it. */
    long cur = ubrk(0);
    if (cur <= 0) return 4;
    long nb = ubrk((unsigned long)cur + 4096);
    if (nb != cur + 4096) return 5;
    unsigned char* h = (unsigned char*)cur;
    for (int i = 0; i < 4096; i++) h[i] = (unsigned char)(i & 0xff);
    for (int i = 0; i < 4096; i++)
        if (h[i] != (unsigned char)(i & 0xff)) return 6;

    return 0;
}
