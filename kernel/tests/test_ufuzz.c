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
#include <sched.h>
#include <mm/pmm.h>
#include <mm/kheap.h>

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

/* ---- U2: Resource Accounting & Reset Correctness ------------------------
 *
 * Reset protocol: snapshot the kernel's memory counters, run the full worker
 * campaign to completion (which forks, faults, hangs, and watchdog-kills
 * workers), let deferred cleanup settle, then snapshot again. Any surviving
 * resource that costs memory — a leaked page table, process slot, fd, vnode,
 * pipe, or heap block — shows up as a non-zero delta in free physical pages or
 * kernel-heap bytes. The aggregate memory oracle therefore catches every
 * resource leak that has a memory cost; finer per-type counters are a later
 * refinement. (Per-process accounting is already proven by
 * proc.spawn_wait_many_no_leak; this adds the adversarial fault/kill paths.)
 */

static void ufuzz_settle(void) {
    /* Let the idle reaper free detached/zombie threads and deferred frames. */
    for (int i = 0; i < 10; i++) sched_sleep_ms(5);
}

KTEST(ufuzz, reset_campaign_no_drift) {
    /* Warm up with one campaign first. The very first /bin/ufuzz run pays a
     * small, bounded one-time cost (~6 pages of lazy init / caches that do NOT
     * recur). Per spec §U2 the reset oracle measures STEADY-STATE drift after
     * settling, not first-touch cost: the invariant that matters is "repeated
     * testcases cause no UNBOUNDED growth", not "zero cost on first touch". */
    int pid = proc_spawn_user("/bin/ufuzz");
    KASSERT_TEST(pid > 0);
    int st = -1;
    sys_waitpid(pid, &st);
    KEXPECT_EQ(st, 0);
    ufuzz_settle();

    size_t pg0 = pmm_get_free_memory();
    size_t hp0 = kheap_get_used();

    /* Four more campaigns = 16 workers + 16 watchdogs, including 4 SIGSEGV
     * faults and 4 SIGKILL (watchdog) timeouts. After settling, every page and
     * heap byte must be back: net-zero drift from the warm baseline, or this
     * flags a real leak in the signal-terminated cleanup path. */
    for (int r = 0; r < 4; r++) {
        pid = proc_spawn_user("/bin/ufuzz");
        KASSERT_TEST(pid > 0);
        st = -1;
        sys_waitpid(pid, &st);
        KEXPECT_EQ(st, 0);
    }

    ufuzz_settle();
    KEXPECT_EQ(pmm_get_free_memory(), pg0);
    KEXPECT_EQ(kheap_get_used(), hp0);
}

/* Negative + positive control for the reset oracle itself: an outstanding
 * allocation MUST register as drift, and a balanced alloc/free MUST NOT. This
 * proves the oracle is not blind (a no-drift result means "no leak", not
 * "oracle can't see"). */
KTEST(ufuzz, reset_oracle_is_sensitive) {
    size_t base = kheap_get_used();
    void* p = kmalloc(8192);
    KASSERT_TEST(p != NULL);
    KEXPECT(kheap_get_used() > base);           /* leak is visible */
    kfree(p);
    KEXPECT_EQ(kheap_get_used(), base);         /* balanced -> no drift */
}
