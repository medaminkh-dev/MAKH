/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - rtc.c
 * Read the motherboard CMOS real-time clock (ports 0x70/0x71) once at boot.
 * We read the fields twice and accept them only when two reads agree and no
 * update is in progress, so we never latch a half-ticked value. BCD vs binary
 * and 12/24-hour are both handled per status register B. See rtc.h.
 */
#include <drivers/rtc.h>
#include <kernel.h>        /* inb/outb */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) { return cmos(0x0A) & 0x80; }

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). Adapted from the
 * well-known civil-to-days algorithm; m in [1,12], d in [1,31]. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t rtc_read_epoch(void) {
    uint8_t sec, min, hour, day, mon, year, regB;
    uint8_t lsec = 0xFF, lmin = 0, lhour = 0, lday = 0, lmon = 0, lyear = 0;

    /* Read until two consecutive, update-quiescent reads agree. */
    for (int tries = 0; tries < 1000000; tries++) {
        while (update_in_progress()) { }
        sec  = cmos(0x00); min  = cmos(0x02); hour = cmos(0x04);
        day  = cmos(0x07); mon  = cmos(0x08); year = cmos(0x09);
        if (sec == lsec && min == lmin && hour == lhour &&
            day == lday && mon == lmon && year == lyear) break;
        lsec = sec; lmin = min; lhour = hour; lday = day; lmon = mon; lyear = year;
    }

    regB = cmos(0x0B);
    if (!(regB & 0x04)) {                 /* values are BCD: unpack */
        #define BCD(v) ((uint8_t)(((v) & 0x0F) + ((v) >> 4) * 10))
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        uint8_t h = hour & 0x7F;          /* keep the PM flag aside for 12h mode */
        h = BCD(h);
        hour = (hour & 0x80) | h;
        #undef BCD
    }
    if (!(regB & 0x02)) {                 /* 12-hour mode: fold PM into 24h */
        int pm = hour & 0x80;
        hour &= 0x7F;
        if (pm && hour != 12) hour += 12;
        if (!pm && hour == 12) hour = 0;
    }

    int64_t full_year = 2000 + (int64_t)year;   /* CMOS year is two digits */
    int64_t days = days_from_civil(full_year, mon ? mon : 1, day ? day : 1);
    return (uint64_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}
