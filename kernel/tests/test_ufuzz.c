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
 * This is a MEMORY-RESET oracle, named honestly for what it measures — not an
 * all-encompassing resource-leak detector. Reset protocol: snapshot the
 * kernel's two aggregate memory counters (free physical pages via pmm, kernel
 * heap bytes via kheap), run the full worker campaign to completion (fork,
 * fault, hang, watchdog-kill), let deferred cleanup settle, then snapshot
 * again and assert net-zero steady-state drift.
 *
 * SCOPE — what this oracle CAN catch: any leak that has a memory cost, because
 * every such object is backed by a pmm page or a kheap allocation — a leaked
 * page table, address space, process/thread slot, kernel stack, file_t (open-
 * file description), vnode, pipe buffer, or heap block will move one of the two
 * counters.
 *
 * LIMITS — what this oracle does NOT, by itself, prove: a leak that costs
 * neither a page nor a heap byte is invisible to it. Examples: exhausting a
 * fixed-size table or id space without freeing memory (fd-number slots, pid
 * bitmap bits), a refcount that is wrong but never reaches a free, or a
 * cross-process reference leak that another counter would show. A no-drift
 * result here therefore means "no memory leak in the tested scenario", not
 * "no resource leak of any kind".
 *
 * REQUIRED per-resource oracles (U2 completeness, tracked separately so they
 * stay necessary rather than assumed): (1) fd-table slot accounting across
 * open/close churn; (2) vnode-cache entry accounting (partly covered by
 * ext2.repeated_resolve_does_not_leak); (3) pipe/socket endpoint accounting;
 * (4) process/pid accounting — partly covered already by
 * proc.spawn_wait_many_no_leak, which this test extends to the adversarial
 * SIGSEGV/SIGKILL cleanup paths. Those are follow-ups, not claims closed here.
 */

static void ufuzz_settle(void) {
    /* Wait for deferred reclamation (the idle reaper frees detached threads and
     * deferred frames) to quiesce: poll until free memory is unchanged across a
     * 10ms window, or a bound elapses. A fixed sleep is flaky under load; this
     * is not — it returns as soon as the counter stops moving. */
    size_t prev = pmm_get_free_memory();
    for (int i = 0; i < 60; i++) {          /* up to ~600ms */
        sched_sleep_ms(10);
        size_t cur = pmm_get_free_memory();
        if (cur == prev) return;            /* quiesced */
        prev = cur;
    }
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

    /* Two more campaigns = 8 workers + 8 watchdogs, including SIGSEGV faults and
     * SIGKILL (watchdog) timeouts. After settling, every page and heap byte
     * must be back: net-zero drift from the warm baseline, or this flags a real
     * leak in the signal-terminated cleanup path. */
    for (int r = 0; r < 2; r++) {
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
