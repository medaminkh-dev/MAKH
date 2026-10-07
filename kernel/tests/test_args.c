/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_args.c
 * Phase 20-E: execve() passes argv on the SysV initial stack. /bin/echo exits
 * with its argc, so a status N means it saw N arguments (argv[0] included).
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <tty.h>

static void feed(const char* s) { for (; *s; s++) tty_input(*s); }

/* Isolates the loader's argv build: execargs execve's /bin/echo with a fixed
 * 4-element argv, becomes echo, and exits with argc == 4. */
KTEST(args, execve_passes_argv) {
    int pid = proc_spawn_user("/bin/execargs");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 4);                       /* {echo, x, y, z} */
}

/* End to end: the shell tokenises a typed line into argv and runs it. */
KTEST(args, shell_runs_command_with_args) {
    tty_init();
    int pid = proc_spawn_user("/bin/sh");
    KASSERT_TEST(pid > 0);
    tty_set_foreground((uint32_t)pid);

    sched_sleep_ms(15);
    feed("/bin/echo a b\n");                      /* 3 words -> argc 3 */
    sched_sleep_ms(40);
    tty_input((char)4);                           /* ^D */

    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 3);                         /* {echo, a, b} */
}
