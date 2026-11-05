/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_ufuzz.c
 * MAKH 1.4.0 / UFUZZ U1 — Ring-3 executor & worker lifecycle.
 *
 * Spawns the real Ring-3 controller /bin/ufuzz and reads back its exit code.
 * The controller runs a fixed four-testcase campaign that exercises every
 * worker-lifecycle outcome the U1 gates require — a normal worker, an
 * expected-error worker, a FAULTING worker (SIGSEGV), and a HANGING worker —
 * and exits 0 only if it observed and handled each as expected. Reaching that
 * clean exit proves two things at once:
 *   - crash containment: the controller survived a worker's SIGSEGV;
 *   - hang detection: the controller's watchdog killed the spinning worker and
 *     classified it as a TIMEOUT, so the campaign completed instead of wedging.
 *
 * Fail-first: remove the watchdog fork in user/ufuzz.c and the HANG testcase
 * wedges the controller in wait4 forever -> /bin/ufuzz never exits ->
 * sys_waitpid here never returns -> the suite times out. (Verified by hand.)
 */
#include <ktest.h>
#include <proc_internal.h>
#include <signal.h>

KTEST(ufuzz, worker_lifecycle_contained) {
    int pid = proc_spawn_user("/bin/ufuzz");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    /* 0 == every lifecycle outcome (normal / expected-error / fault / hang)
     * was observed and handled; the controller was not dropped by either the
     * faulting or the hanging worker. */
    KEXPECT_EQ(status, 0);
}
