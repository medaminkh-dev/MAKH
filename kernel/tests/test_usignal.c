/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_usignal.c
 * Phase 20-G: user-installed signal handlers. Each program encodes the whole
 * outcome in its exit status, so the kernel just reads it back.
 */

#include <ktest.h>
#include <proc_internal.h>

/* /bin/sigtest installs a SIGTERM handler, raises it, and exits 42 iff the
 * handler ran and sigreturn resumed the mainline. */
KTEST(usignal, handler_runs_and_sigreturn_resumes) {
    int pid = proc_spawn_user("/bin/sigtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}

/* /bin/sigmask proves a blocked signal stays pending until unblocked: the
 * mainline digit (1) lands before the handler digit (2), so the status is 12. */
KTEST(usignal, sigprocmask_defers_delivery) {
    int pid = proc_spawn_user("/bin/sigmask");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 12);
}
