/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - rtc.h
 * CMOS real-time clock: a one-shot read at boot gives a real Unix epoch, which
 * backs CLOCK_REALTIME and gettimeofday (Phase 20-M).
 */
#ifndef MAKHOS_RTC_H
#define MAKHOS_RTC_H

#include <types.h>

/* Read the CMOS RTC and return seconds since the Unix epoch (UTC). */
uint64_t rtc_read_epoch(void);

/* Bounded wait on the CMOS update-in-progress bit: poll busy() until it reads
 * 0 (clear), or `limit` polls elapse. Returns 0 (clear) or -1 (timeout), so a
 * wedged CMOS can never hang the boot. Exposed for KTEST (T-L01). */
int rtc_wait_clear(int (*busy)(void), uint32_t limit);

#endif /* MAKHOS_RTC_H */
