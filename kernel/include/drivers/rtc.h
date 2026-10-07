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

#endif /* MAKHOS_RTC_H */
