/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise the time + randomness syscalls and exit 77 iff all behave:
 *   - clock_gettime(MONOTONIC) advances across a nanosleep,
 *   - getrandom fills the buffer, non-zero, and two draws differ. */
#include "usys.h"

int umain(void) {
    struct timespec a, b;
    if (uclock_gettime(CLOCK_MONOTONIC, &a) < 0) return 1;

    struct timespec req = { 0, 30 * 1000 * 1000 };   /* 30 ms */
    if (unanosleep(&req, 0) < 0) return 2;

    if (uclock_gettime(CLOCK_MONOTONIC, &b) < 0) return 3;
    long dms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    if (dms < 10) return 4;                          /* clock must advance */

    unsigned char r1[16] = {0}, r2[16] = {0};
    if (ugetrandom(r1, sizeof(r1), 0) != (long)sizeof(r1)) return 5;
    if (ugetrandom(r2, sizeof(r2), 0) != (long)sizeof(r2)) return 6;
    int allzero = 1, same = 1;
    for (int i = 0; i < 16; i++) {
        if (r1[i]) allzero = 0;
        if (r1[i] != r2[i]) same = 0;
    }
    if (allzero) return 7;                           /* not all zero */
    if (same) return 8;                              /* two draws differ */

    return 77;
}
