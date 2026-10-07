/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Thread-local storage via arch_prctl(ARCH_SET_FS): point FS at a block, read
 * it back through the %fs segment (what a TLS access compiles to), and confirm
 * ARCH_GET_FS reports the same base. Exits 55 on full success. */
#include "usys.h"

int umain(void) {
    static unsigned long tls = 55;              /* "thread pointer" block */

    if (uset_fs(&tls) < 0) return 1;

    unsigned long via_fs;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(via_fs));  /* read tls through FS */
    if (via_fs != 55) return 2;

    unsigned long got = 0;
    if (uarch_prctl(ARCH_GET_FS, (unsigned long)&got) < 0) return 3;
    if (got != (unsigned long)&tls) return 4;

    return 55;                                   /* SET_FS + %fs read + GET_FS ok */
}
