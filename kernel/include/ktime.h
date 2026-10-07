/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_KTIME_H
#define MAKHOS_KTIME_H

#include <types.h>

/**
 * ktime.h - minimal monotonic time, backed by the PIT tick counter.
 * Resolution is one timer tick (10ms at 100Hz); good enough for timeouts.
 */

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* Monotonic milliseconds since boot. */
uint64_t clock_now_ms(void);

/* Wall-clock milliseconds since the Unix epoch (RTC-anchored, Phase 20-M). */
uint64_t clock_now_realtime_ms(void);

/* Read the CMOS RTC once and anchor the wall clock. Call once at boot. */
void ktime_init_realtime(void);

/* POSIX-style clock read (monotonic only; realtime aliases it). */
int clock_gettime(int clk, struct timespec* ts);

/* Convert a timespec to absolute milliseconds (for timeout math). */
uint64_t timespec_to_ms(const struct timespec* ts);

#endif /* MAKHOS_KTIME_H */
