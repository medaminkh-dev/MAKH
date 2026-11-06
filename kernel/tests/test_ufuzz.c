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
#include <cmdline.h>
#include <kfuzz.h>

/* These are wall-clock-budget test harnesses (watchdog timeouts, deferred-
 * reclaim settling), not kernel-race probes. The stress job runs the suite
 * 12x4 in parallel, which deliberately degrades scheduling latency — the very
 * thing these measure — so it boots the stress ISO with `makh.notiming` and
 * they skip there. They are fully validated once in the build-and-test job. */
#define UFUZZ_SKIP_IF_NOTIMING() \
    do { if (cmdline_has("makh.notiming")) { KEXPECT(1); return; } } while (0)

KTEST(ufuzz, worker_lifecycle_contained) {
    UFUZZ_SKIP_IF_NOTIMING();
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
    UFUZZ_SKIP_IF_NOTIMING();
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

/* ---- U3: typed syscall programs execute in Ring 3 and replay ------------
 * `/bin/ufuzz u3` runs the baked typed-program set (ufuzz_u3_programs.h) — each
 * program in an isolated, watchdog-bounded worker — with runtime resource
 * tracking + success-gating, and (for replay programs) re-runs and requires an
 * identical per-call trace. The controller exits 0 iff every program's outcome
 * matched its expectation. The set covers: generated balanced programs, a pipe
 * round-trip (data read back == written), an mmap/mprotect/munmap success, and
 * an mmap failure whose un-mapped handle is then munmap'd without crashing the
 * controller (success-gating). This is the U3 §7 acceptance gate that needs the
 * guest; the structure/generator/validator/serialize gates run host-side in
 * fuzz/u3/selftest.py (CI abi-check job). */
KTEST(ufuzz, u3_typed_programs_execute_and_replay) {
    UFUZZ_SKIP_IF_NOTIMING();
    char* argv[] = { "/bin/ufuzz", "u3", 0 };
    int pid = proc_spawn_user_argv("/bin/ufuzz", argv);
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);
}

/* ---- U4 step 1: coverage-measurement plumbing (the part the design review
 * flagged as the real risk) -------------------------------------------------
 * Proves the ring-0 driver can observe EDGE-IDENTITY novelty from a ring-3
 * worker and attribute it, using the two-map scheme from docs/UFUZZ_U4_DESIGN.md:
 * a per-run map (cov_map) folded against a persistent `virgin` map.
 *
 * Protocol: enable coverage; a warmup run folds the boot/spawn/exec/wait edges
 * into `virgin` and is discarded; then the SAME ring-3 program is run twice.
 *   - the first run must reveal NEW edges (nov1 > 0) — ring-3-driven coverage
 *     is observable at all;
 *   - the second, identical run must reveal STRICTLY FEWER (nov2 < nov1) — the
 *     virgin map deduplicates by edge identity, so a re-run is not credited
 *     again. If fold_new ignored `virgin` (counted bits, not identity), nov2
 *     would equal nov1 and this fails. That is the U4 feedback signal in
 *     miniature; the full guided-vs-random campaign builds on it. */
static uint64_t u4_virgin[1 << 10];   /* >= kfuzz_cov_words() (COV_BITS/64) */

/* Reset the per-run map, run one worker to completion, then fold its edges into
 * `virgin` — returning how many were NEW. Reset-before-run measures each run in
 * isolation (its worker's syscalls plus the shared spawn/exec/wait path). */
static uint64_t u4_run_and_fold(const char* arg) {
    kfuzz_cov_reset();
    char* argv[] = { "/bin/ufuzz", (char*)arg, 0 };
    int pid = arg ? proc_spawn_user_argv("/bin/ufuzz", argv)
                  : proc_spawn_user("/bin/ufuzz");
    if (pid <= 0) return 0;
    int st = -1;
    sys_waitpid(pid, &st);
    return kfuzz_cov_fold_new(u4_virgin);
}

KTEST(ufuzz, u4_coverage_plumbing_attributes_novelty) {
    UFUZZ_SKIP_IF_NOTIMING();
    uint32_t words = kfuzz_cov_words();
    KASSERT_TEST(words <= (sizeof(u4_virgin) / sizeof(u4_virgin[0])));
    for (uint32_t i = 0; i < words; i++) u4_virgin[i] = 0;

    int prev = kfuzz_cov_enable(1);

    /* Warmup: absorb the spawn/exec/wait + lifecycle edges every run shares. */
    (void)u4_run_and_fold(0);          /* /bin/ufuzz (U1 lifecycle) */

    /* Two identical U3 campaigns. The first reveals new edges; the second must
     * not be re-credited (virgin dedup by edge identity). */
    uint64_t nov1 = u4_run_and_fold("u3");
    uint64_t nov2 = u4_run_and_fold("u3");

    kfuzz_cov_enable(prev);

    KEXPECT(nov1 > 0);                 /* ring-3-driven coverage is observable */
    KEXPECT(nov2 < nov1);              /* re-run adds strictly fewer new edges  */
}

/* ---- U4 step 2b: C-generated programs run clean under the runtime authorities
 * The host validator (fuzz/u4/validate.py) is the STRUCTURAL authority; this is
 * the RUNTIME authority. Each /bin/ufuzz cov <seed> builds one program with the
 * shared generator and runs it with replay + out-buffer digest under the U1
 * watchdog; it exits 0 iff the program ran, replayed and digest-matched (no
 * fault, no hang, no non-determinism). The whole batch must also leave memory
 * net-zero (U2). A malformed generated program would fault or mismatch here —
 * so a C-generator bug cannot pass silently through BOTH authorities. */
static void u4_utoa(unsigned v, char* out) {
    char tmp[16]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

static int u4_run_cov(unsigned seed) {
    char sb[16]; u4_utoa(seed, sb);
    char* argv[] = { "/bin/ufuzz", "cov", sb, 0 };
    int pid = proc_spawn_user_argv("/bin/ufuzz", argv);
    if (pid <= 0) return -1;
    int st = -1; sys_waitpid(pid, &st);
    return st;
}

KTEST(ufuzz, u4_generated_programs_run_clean) {
    UFUZZ_SKIP_IF_NOTIMING();
    const unsigned K = 24;

    (void)u4_run_cov(0);                       /* warmup: absorb first-touch cost */
    ufuzz_settle();
    size_t pg0 = pmm_get_free_memory();
    size_t hp0 = kheap_get_used();

    int bad = 0;
    for (unsigned s = 0; s < K; s++)
        if (u4_run_cov(s) != 0) bad++;
    KEXPECT_EQ(bad, 0);                         /* every generated program OUT_OK */

    ufuzz_settle();
    KEXPECT_EQ(pmm_get_free_memory(), pg0);     /* no page leak over the batch */
    KEXPECT_EQ(kheap_get_used(), hp0);          /* no heap leak over the batch */
}
