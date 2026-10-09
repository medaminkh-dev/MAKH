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

/* U1-b: busybox applets reached as /bin/<name> symlinks. The kernel resolves
 * the symlink to /bin/busybox during execve, while argv[0]'s basename selects
 * the applet (multi-call dispatch) — a user types `ls`, not `busybox ls`. */
KTEST(busybox, applets_via_symlink) {
    /* /bin/true and /bin/false -> busybox: the applet decides the exit status. */
    char* at[] = { "true",  0 };
    int p1 = proc_spawn_user_argv("/bin/true", at);
    KASSERT_TEST(p1 > 0);
    int s1 = -1; sys_waitpid(p1, &s1);
    KEXPECT_EQ(s1, 0);

    char* af[] = { "false", 0 };
    int p2 = proc_spawn_user_argv("/bin/false", af);
    KASSERT_TEST(p2 > 0);
    int s2 = -1; sys_waitpid(p2, &s2);
    KEXPECT_EQ(s2, 1);

    /* /bin/ash -> busybox: busybox's shell reached by a conventional path
     * (MAKH keeps its own /bin/sh). */
    char* ash[] = { "ash", "-c", "exit $((5+2))", 0 };
    int p3 = proc_spawn_user_argv("/bin/ash", ash);
    KASSERT_TEST(p3 > 0);
    int s3 = -1; sys_waitpid(p3, &s3);
    KEXPECT_EQ(s3, 7);
}
