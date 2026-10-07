/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - ktime.c
 * Monotonic time from the PIT tick counter, plus a wall clock anchored to the
 * CMOS RTC at boot (Phase 20-M). CLOCK_MONOTONIC is ticks-since-boot;
 * CLOCK_REALTIME is the boot epoch plus the same monotonic offset.
 */

#include <ktime.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>

/* Unix-epoch milliseconds that correspond to monotonic time 0 (boot). */
static uint64_t realtime_base_ms;

uint64_t clock_now_ms(void) {
    return timer_get_ticks() * 1000ULL / TIMER_FREQUENCY;
}

/* Read the RTC once and anchor the wall clock. Called from kernel_main. */
void ktime_init_realtime(void) {
    uint64_t epoch_ms = rtc_read_epoch() * 1000ULL;
    uint64_t mono     = clock_now_ms();
    realtime_base_ms  = epoch_ms > mono ? epoch_ms - mono : epoch_ms;
}

uint64_t clock_now_realtime_ms(void) {
    return realtime_base_ms + clock_now_ms();
}

int clock_gettime(int clk, struct timespec* ts) {
    if (!ts) return -1;
    uint64_t ms = (clk == CLOCK_REALTIME) ? clock_now_realtime_ms() : clock_now_ms();
    ts->tv_sec  = (int64_t)(ms / 1000);
    ts->tv_nsec = (int64_t)((ms % 1000) * 1000000);
    return 0;
}

uint64_t timespec_to_ms(const struct timespec* ts) {
    if (!ts) return 0;
    return (uint64_t)ts->tv_sec * 1000 + (uint64_t)ts->tv_nsec / 1000000;
}
