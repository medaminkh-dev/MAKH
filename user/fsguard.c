/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* T-S01 regression harness: a ring-3 program MUST NOT be able to fault the
 * kernel by handing it a non-canonical (or kernel-half) FS base. Each bad
 * arch_prctl(ARCH_SET_FS, ...) must be REJECTED (negative return), never
 * wrmsr'd in ring 0 — and a legal user-half base must still work and read back.
 *
 * Exits 55 iff every bad base was rejected AND the good base round-trips.
 * Fail-first: without the do_arch_prctl canonical guard, the FIRST bad SET_FS
 * below #GPs on wrmsr in ring 0 -> exception_report (noreturn) -> the kernel
 * dies, so this program never exits and the suite times out. */
#include "usys.h"

int umain(void) {
    /* Bases that must be refused:
     *  - 0x0000800000000000: first address past the user half (non-canonical);
     *  - 0x8000000000000000: bit 63 set, bits [62:48] clear (non-canonical);
     *  - 0xDEADBEEF00000000: bits [63:48] nonzero (non-canonical);
     *  - 0xFFFF800000000000: kernel-half canonical (policy: user FS may not
     *                        point into the kernel half). */
    unsigned long bad[] = {
        0x0000800000000000UL, 0x8000000000000000UL,
        0xDEADBEEF00000000UL, 0xFFFF800000000000UL,
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (uarch_prctl(ARCH_SET_FS, bad[i]) >= 0)
            return 10 + (int)i;                 /* a bad base was NOT rejected */

    /* A legal user-half base must still succeed, be reachable through %fs, and
     * read back identically via ARCH_GET_FS (the guard must not over-reject). */
    static unsigned long tls = 0xA5A5A5A5;
    if (uset_fs(&tls) < 0) return 20;
    unsigned long via_fs = 0;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(via_fs));
    if (via_fs != 0xA5A5A5A5) return 21;
    unsigned long got = 0;
    if (uarch_prctl(ARCH_GET_FS, (unsigned long)&got) < 0) return 22;
    if (got != (unsigned long)&tls) return 23;

    return 55;
}
