/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_job.c
 * Phase 20-F: job control. Two angles:
 *   1. the raw syscalls (setpgid/getpgrp/ioctl/tcsetpgrp) from ring 3, and
 *   2. end to end — Ctrl+C must kill the running job, not the shell.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <tty.h>

static void feed(const char* s) { for (; *s; s++) tty_input(*s); }

/* /bin/jobtest runs the group/terminal syscalls and exits 0 iff all behaved;
 * a non-zero status names the first step that failed. */
KTEST(job, syscalls_roundtrip) {
    tty_init();
    int pid = proc_spawn_user("/bin/jobtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);
}

/* The decisive job-control test. The shell runs /bin/spinner (never returns on
 * its own) as a foreground job in its own group; ^C must hit that group and
 * leave the shell alive. We prove the shell survived by having it run a second
 * command (/bin/hello, which exits 42) afterwards and reading that back: if ^C
 * had instead killed the shell, waitpid would see 130, not 42. */
KTEST(job, ctrl_c_kills_job_not_shell) {
    tty_init();
    int pid = proc_spawn_user("/bin/sh");
    KASSERT_TEST(pid > 0);
    tty_set_foreground((uint32_t)pid);

    sched_sleep_ms(15);
    feed("/bin/spinner\n");                      /* foreground job, loops forever */
    sched_sleep_ms(60);                          /* let it take the terminal      */
    tty_input((char)3);                          /* ^C -> job's group, not shell  */
    sched_sleep_ms(60);                          /* job dies; shell re-prompts     */
    feed("/bin/hello\n");                        /* shell still alive? run a cmd   */
    sched_sleep_ms(60);
    tty_input((char)4);                          /* ^D: shell exits with last st  */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);                      /* 42 => shell survived ^C        */
}
