/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_time.c
 * Phase 20-I: time + randomness syscalls from ring 3. /bin/timetest checks that
 * clock_gettime advances across a nanosleep and that getrandom returns distinct
 * non-zero draws, exiting 77 on full success.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <krandom.h>

KTEST(time, clock_nanosleep_getrandom_from_userspace) {
    int pid = proc_spawn_user("/bin/timetest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 77);
}

/* A direct kernel check of the RNG: 256 draws must not collide (a stuck or
 * zero generator would repeat immediately). */
KTEST(time, krandom_draws_are_distinct) {
    uint64_t seen[256];
    int dup = 0;
    for (int i = 0; i < 256; i++) {
        seen[i] = krandom_u64();
        for (int j = 0; j < i; j++) if (seen[j] == seen[i]) dup = 1;
    }
    KEXPECT_EQ(dup, 0);
}
