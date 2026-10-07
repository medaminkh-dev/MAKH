/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_pipe.c
 * Phase 20-K: pipes, dup2, and fd inheritance. /bin/pipetest drives the raw
 * mechanism (pipe write/read + EOF + dup2 redirection, exit 55); the shell test
 * proves an end-to-end pipeline — `echo abc | countin` sends 4 bytes through a
 * pipe, so the shell exits with 4.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <tty.h>

static void feed(const char* s) { for (; *s; s++) tty_input(*s); }

KTEST(pipe, pipe_dup2_fork_mechanism) {
    int pid = proc_spawn_user("/bin/pipetest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 55);
}

/* End to end: the shell runs a two-stage pipeline. echo writes "abc\n" (4
 * bytes) into a pipe; countin reads to EOF and exits with the byte count, so
 * the shell's last status — reported on ^D — is 4. */
KTEST(pipe, shell_runs_a_pipeline) {
    tty_init();
    int pid = proc_spawn_user("/bin/sh");
    KASSERT_TEST(pid > 0);
    tty_set_foreground((uint32_t)pid);

    sched_sleep_ms(15);
    feed("/bin/echo abc | /bin/countin\n");
    sched_sleep_ms(60);
    tty_input((char)4);                          /* ^D */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 4);
}
