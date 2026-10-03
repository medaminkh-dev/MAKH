/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - ktime.c
 * Monotonic time from the PIT tick counter.
 */

#include <ktime.h>
#include <drivers/timer.h>

uint64_t clock_now_ms(void) {
    return timer_get_ticks() * 1000ULL / TIMER_FREQUENCY;
}

int clock_gettime(int clk, struct timespec* ts) {
    (void)clk;  /* monotonic; realtime aliases it (no RTC yet) */
    if (!ts) return -1;
    uint64_t ms = clock_now_ms();
    ts->tv_sec = (int64_t)(ms / 1000);
    ts->tv_nsec = (int64_t)((ms % 1000) * 1000000);
    return 0;
}

uint64_t timespec_to_ms(const struct timespec* ts) {
    if (!ts) return 0;
    return (uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000;
}
