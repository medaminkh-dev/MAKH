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
