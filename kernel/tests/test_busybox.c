/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_busybox.c
 * Phase 20-O2 (F20-c): a real, unmodified busybox — built from source as a
 * static-PIE against musl — runs its ash shell on MAKH. The shell parses a
 * script, does arithmetic, prints through stdout, and exits with a computed
 * status. /bin/busybox is spawned as `busybox sh -c '...'`; a status of 42
 * means ash tokenised the line, evaluated $((6*7)), ran echo, and exited with
 * the arithmetic result — the whole shell path end to end on our syscall ABI.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <proc.h>

KTEST(busybox, ash_runs_a_script) {
    char* argv[] = { "/bin/busybox", "sh", "-c",
                     "echo bb-$((6*7)); exit $((6*7))", 0 };
    int pid = proc_spawn_user_argv("/bin/busybox", argv);
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}
