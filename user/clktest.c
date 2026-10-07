/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Wall clock sanity: CLOCK_REALTIME is a real epoch (far past 2020) and far
 * ahead of CLOCK_MONOTONIC (seconds since boot), gettimeofday agrees, and the
 * clock advances across a sleep. Exit 66 on success. */
#include "usys.h"

int umain(void) {
    struct timespec rt, mono;
    if (uclock_gettime(CLOCK_REALTIME, &rt) < 0) return 1;
    if (uclock_gettime(CLOCK_MONOTONIC, &mono) < 0) return 2;

    if (rt.tv_sec < 1600000000L) return 3;           /* must be after 2020-09 */
    if (rt.tv_sec <= mono.tv_sec + 1000000L) return 4; /* epoch >> seconds-since-boot */

    struct timeval tv;
    if (ugettimeofday(&tv) < 0) return 5;
    long d = tv.tv_sec - rt.tv_sec;
    if (d < -2 || d > 2) return 6;                   /* gettimeofday ~ clock_gettime */

    struct timespec a, b;
    uclock_gettime(CLOCK_REALTIME, &a);
    usleep_ms(30);
    uclock_gettime(CLOCK_REALTIME, &b);
    long dms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
    if (dms < 10) return 7;                          /* wall clock advanced */

    return 66;
}
