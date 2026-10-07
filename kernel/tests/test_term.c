/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_term.c
 * Phase 20-D (controlling terminal): a user process's read() on stdin blocks in
 * the line discipline until a line arrives, then returns it. Input is injected
 * from the kernel via tty_input() (the same entry point the keyboard IRQ uses),
 * so the whole keyboard -> tty -> blocked-reader path is exercised headless.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <signal.h>
#include <tty.h>

static void feed(const char* s) { for (; *s; s++) tty_input(*s); }

/* A process blocked in read() wakes and returns the line once it is typed. */
KTEST(term, read_blocks_until_a_line_arrives) {
    tty_init();                                 /* isolate from other tty tests */
    int pid = proc_spawn_user("/bin/readline");
    KASSERT_TEST(pid > 0);

    sched_sleep_ms(15);                         /* let it reach the blocking read */
    feed("hi\n");                               /* now a line is available */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 3);                       /* "hi\n" is 3 bytes */
}

/* Ctrl+C kills the process reading the terminal: the ^C raises SIGINT on the
 * foreground group, and the fatal signal is delivered on the way back to ring 3
 * (here, out of the interrupted read()), terminating it with 128 + SIGINT. */
KTEST(term, ctrl_c_kills_the_foreground_reader) {
    tty_init();
    int pid = proc_spawn_user("/bin/readline");
    KASSERT_TEST(pid > 0);
    tty_set_foreground((uint32_t)pid);          /* its pgid == its pid */

    sched_sleep_ms(15);                          /* let it block in read() */
    tty_input((char)3);                          /* ^C */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 128 + SIGINT);            /* killed by SIGINT -> 130 */
}

/* ^D on an empty line is end-of-file: read() returns 0. */
KTEST(term, ctrl_d_on_empty_line_is_eof) {
    tty_init();
    int pid = proc_spawn_user("/bin/readline");
    KASSERT_TEST(pid > 0);

    sched_sleep_ms(15);
    tty_input((char)4);                          /* ^D */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);                        /* EOF -> 0 bytes */
}

/* The whole interactive loop in miniature: the shell reads a command line from
 * the terminal, fork+execve's it, waits, and on EOF exits with its status. */
KTEST(term, shell_runs_a_typed_command) {
    tty_init();
    int pid = proc_spawn_user("/bin/sh");
    KASSERT_TEST(pid > 0);
    tty_set_foreground((uint32_t)pid);

    sched_sleep_ms(15);                          /* shell blocks on its first read */
    feed("/bin/hello\n");                        /* "type" a command */
    sched_sleep_ms(40);                          /* it forks+execs hello, re-blocks */
    tty_input((char)4);                          /* ^D: end the session */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);                       /* exit status of the last command */
}
