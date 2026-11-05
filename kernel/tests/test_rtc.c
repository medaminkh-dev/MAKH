/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_rtc.c
 * Phase 20-M: RTC-anchored wall clock. A direct kernel check that the realtime
 * epoch is sane and far ahead of monotonic, plus /bin/clktest exercising
 * CLOCK_REALTIME / gettimeofday / advance-across-sleep from ring 3 (exit 66).
 */

#include <ktest.h>
#include <proc_internal.h>
#include <ktime.h>
#include <drivers/rtc.h>

/* -------- T-L01: the CMOS update-in-progress wait is bounded -------- */

static int rt_always_busy(void) { return 1; }          /* a wedged CMOS */
static int rt_countdown;
static int rt_busy_then_clear(void) { return rt_countdown-- > 0; }

KTEST(rtc, uip_wait_is_bounded) {
    /* A CMOS whose update-in-progress bit never clears must NOT hang boot:
     * the bounded wait returns -1 (timeout). Revert the fix (unbounded
     * `while (update_in_progress()) {}`) and this test spins forever. */
    KEXPECT_EQ(rtc_wait_clear(rt_always_busy, 100000u), -1);

    /* A CMOS that settles after a few polls succeeds (returns 0). */
    rt_countdown = 8;
    KEXPECT_EQ(rtc_wait_clear(rt_busy_then_clear, 100000u), 0);
}

KTEST(rtc, realtime_epoch_is_sane) {
    uint64_t rt_ms = clock_now_realtime_ms();
    uint64_t mono_ms = clock_now_ms();
    KEXPECT_NE(rt_ms / 1000 > 1600000000ULL, 0);      /* after 2020-09 */
    KEXPECT_NE(rt_ms > mono_ms + 1000000000ULL, 0);   /* epoch >> since-boot */
}

KTEST(rtc, wall_clock_from_userspace) {
    int pid = proc_spawn_user("/bin/clktest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 66);
}
