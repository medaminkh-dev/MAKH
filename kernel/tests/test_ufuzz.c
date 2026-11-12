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
#include <klog.h>

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

static int u4_run_cov(int mode, unsigned base, unsigned mut) {
    char ds[16], bs[16], ms[16];
    u4_utoa((unsigned)mode, ds); u4_utoa(base, bs); u4_utoa(mut, ms);
    char* argv[] = { "/bin/ufuzz", "cov", ds, bs, ms, 0 };   /* cov <mode> <base> <mut> */
    int pid = proc_spawn_user_argv("/bin/ufuzz", argv);
    if (pid <= 0) return -1;
    int st = -1; sys_waitpid(pid, &st);
    return st;
}

KTEST(ufuzz, u4_generated_programs_run_clean) {
    UFUZZ_SKIP_IF_NOTIMING();
    const unsigned K = 24;

    for (int mode = 0; mode <= 1; mode++) {     /* 0 skeleton, 1 ext2 block-map growth */
        (void)u4_run_cov(mode, 0, 0);           /* warmup: absorb first-touch cost */
        ufuzz_settle();
        size_t pg0 = pmm_get_free_memory();
        size_t hp0 = kheap_get_used();

        int bad = 0;
        for (unsigned s = 0; s < K; s++)
            if (u4_run_cov(mode, s, s) != 0) bad++;  /* base==mut sweeps the space */
        KEXPECT_EQ(bad, 0);                     /* every generated program OUT_OK */

        ufuzz_settle();
        KEXPECT_EQ(pmm_get_free_memory(), pg0); /* no page leak over the batch */
        KEXPECT_EQ(kheap_get_used(), hp0);      /* no heap leak (vnode/inode) over batch */
    }
}

/* ---- U4 step 3: coverage-guided campaign (measurement + guided-vs-random) --
 * The feedback loop: each iteration spawns /bin/ufuzz cov <seed>, folds the new
 * edges against `virgin`, and — in GUIDED mode — keeps a seed that found new
 * coverage so later iterations can mutate it. RANDOM mode always draws fresh.
 * The exit status carries the outcome class (see ufuzz.c: 0 OK, 2 fault,
 * 3 timeout). This KTEST prints both passes' metrics; the hard guided-vs-random
 * assertion and its MARGIN are set from the printed numbers (design review #4).
 */
#define U4_OUT_FAULT   2
#define U4_OUT_TIMEOUT 3
#define U4_CORPUS_MAX  64

static unsigned long u4_mrng;                   /* campaign meta-PRNG state */
static unsigned u4_mnext(void) {
    u4_mrng = u4_mrng * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned)(u4_mrng >> 33);
}

struct u4_campaign_result { uint64_t bits; int executed, corpus_n, ok, crashes, timeouts; };

/* Run K iterations in guided or random mode from a fixed meta-seed; return the
 * number of NEW coverage edges discovered over the K iterations (warmup folded
 * and discarded first, so shared spawn/exec edges don't count). */
static void u4_campaign(int mode, int guided, unsigned meta_seed, int K,
                        struct u4_campaign_result* out) {
    uint32_t words = kfuzz_cov_words();
    for (uint32_t i = 0; i < words; i++) u4_virgin[i] = 0;
    unsigned corpus_base[U4_CORPUS_MAX]; int corpus_n = 0;   /* fruitful SKELETONS */
    u4_mrng = meta_seed | 1UL;

    kfuzz_cov_reset();                           /* warmup: fold shared edges */
    (void)u4_run_cov(mode, 0xABCD, 0);
    (void)kfuzz_cov_fold_new(u4_virgin);

    uint64_t bits = 0; int ok = 0, crashes = 0, timeouts = 0;
    for (int i = 0; i < K; i++) {
        unsigned base, mut;
        if (guided && corpus_n > 0 && (u4_mnext() & 1)) {
            base = corpus_base[u4_mnext() % (unsigned)corpus_n];  /* KEEP skeleton */
            mut  = u4_mnext();                                    /* VARY selectors */
        } else {
            base = u4_mnext(); mut = u4_mnext();                  /* fresh */
        }

        kfuzz_cov_reset();
        int st = u4_run_cov(mode, base, mut);
        uint64_t nov = kfuzz_cov_fold_new(u4_virgin);
        bits += nov;
        if (st == 0) ok++;                       /* ran, replayed, digest-matched */
        else if (st == U4_OUT_FAULT) crashes++;
        else if (st == U4_OUT_TIMEOUT) timeouts++;
        if (st != 0)                             /* attribute every non-clean run */
            kprintf("u4: NOT-CLEAN mode=%d base=%u mut=%u status=%d\n", mode, base, mut, st);
        /* Keep the SKELETON that found new coverage, so guided can walk its
         * selector neighbourhood (do_arch_prctl switch arms) on later draws. */
        if (guided && nov > 0 && corpus_n < U4_CORPUS_MAX) corpus_base[corpus_n++] = base;
    }
    out->bits = bits; out->executed = K; out->corpus_n = corpus_n;
    out->ok = ok; out->crashes = crashes; out->timeouts = timeouts;
}

KTEST(ufuzz, u4_coverage_guided_campaign) {
    UFUZZ_SKIP_IF_NOTIMING();
    const int K = 60;
    const unsigned META = 0xC0FFEE;

    int prev = kfuzz_cov_enable(1);
    struct u4_campaign_result g = {0}, g2 = {0}, r = {0};
    u4_campaign(0, 1, META, K, &g);              /* skeleton: guided            */
    u4_campaign(0, 1, META, K, &g2);             /* skeleton: guided, repeated  */
    u4_campaign(0, 0, META, K, &r);              /* skeleton: random            */
    kfuzz_cov_enable(prev);

    kprintf("u4: guided bits=%llu (repeat %llu) corpus=%d crashes=%d timeouts=%d"
            " | random bits=%llu corpus=%d\n",
            g.bits, g2.bits, g.corpus_n, g.crashes, g.timeouts, r.bits, r.corpus_n);

    /* HONEST RESULTS (design review #4: measure, don't assume). Two measured
     * facts shape what this test may assert:
     *
     * 1. Seed-level feedback does NOT beat random here. The generator is a pure
     *    function of the seed, so mutating a fruitful seed (seed ^ rand) yields
     *    an UNRELATED program, not a variant — the corpus confers no structural
     *    advantage, and guided vs random differ only by sample variance
     *    (measured ~33 vs ~49 edges, K=60). Real coverage-guided feedback needs
     *    PROGRAM-STRUCTURE mutation (tweak/splice a kept program), deferred — see
     *    docs/UFUZZ_U4_DESIGN.md §8. So we NEVER assert guided >= random.
     *
     * 2. The edge COUNT is not bit-exact reproducible (measured 33 then 39 for
     *    the same meta-seed): instrumented allocator/signal paths (kheap, pmm,
     *    signal are in COV_SOURCES) branch on evolving global state and IRQ
     *    timing. The SEED sequence and the generated PROGRAMS are deterministic
     *    (a pure PRNG + pure generator); the resulting coverage count is not. So
     *    we NEVER assert an exact count or g.bits == g2.bits.
     *
     * What IS robustly true, and all this test claims: the campaign explores
     * real coverage, forms a corpus of coverage-increasing seeds, and every
     * generated program is clean under the runtime oracles (U1 no fault/hang). */
    KEXPECT(g.bits > 0);                          /* real coverage discovered      */
    KEXPECT(r.bits > 0);
    KEXPECT(g.corpus_n > 0);                      /* a corpus actually forms       */
    KEXPECT_EQ(g.ok, K);                          /* EVERY program clean (OK): no  */
    KEXPECT_EQ(r.ok, K);                          /* fault/hang/replay/digest fail */
}

/* ---- U4.2 P1: ext2 block-map growth ---------------------------------------
 * The generator's ext2 mode grows a /mnt file by positioned writes whose
 * offsets span the ext2 block-map tiers (mode 1: direct / single-indirect /
 * double-indirect; mode 2: direct tier only — the control). Each program is
 * open(O_CREAT|O_TRUNC) ... close + unlink, so every run starts from an empty
 * file and the snapshot=on disk never accumulates state. */

/* Run `n` programs of `mode` from seeds (base0+i, mut0+i), folding each run's
 * coverage into u4_virgin; return the NEW edges over the batch and count the
 * clean (OUT_OK: ran, replayed, digest-matched) runs into *ok. */
static uint64_t u4_fold_batch(int mode, unsigned base0, unsigned mut0, int n, int* ok) {
    uint64_t nov = 0;
    for (int i = 0; i < n; i++) {
        kfuzz_cov_reset();
        if (u4_run_cov(mode, base0 + (unsigned)i, mut0 + (unsigned)i) == 0) (*ok)++;
        nov += kfuzz_cov_fold_new(u4_virgin);
    }
    return nov;
}

/* Milestone, as a CONTROLLED comparison (coverage counts carry allocator/IRQ
 * noise — see the campaign test above — so "new edges > 0" alone would prove
 * nothing): saturate the direct tier, measure a direct-only NOISE FLOOR with
 * fresh seeds, then run deep-tier programs. If the single/double-indirect paths
 * (alloc_indirect levels 1-2, the indirect read walk, free_indirect_from on
 * unlink) were not genuinely reached, `deep` would sit at the noise floor. */
KTEST(ufuzz, u4_ext2_tiers_reach_new_coverage) {
    UFUZZ_SKIP_IF_NOTIMING();
    const int N = 12;
    int prev = kfuzz_cov_enable(1);
    uint32_t words = kfuzz_cov_words();
    for (uint32_t i = 0; i < words; i++) u4_virgin[i] = 0;

    int ok = 0;
    (void)u4_fold_batch(2, 0xD100, 0xD100, 1, &ok);            /* warmup: shared paths  */
    uint64_t sat  = u4_fold_batch(2, 0xA000, 0xB000, N, &ok);  /* A: saturate direct    */
    uint64_t flr  = u4_fold_batch(2, 0xC000, 0xD000, N, &ok);  /* B: direct noise floor */
    uint64_t deep = u4_fold_batch(1, 0xE000, 0xF000, N, &ok);  /* C: + indirect tiers   */
    kfuzz_cov_enable(prev);

    kprintf("u4-ext2 tiers: direct-saturate=%llu direct-floor=%llu deep-tiers=%llu "
            "clean=%d/%d\n", sat, flr, deep, ok, 3 * N + 1);
    KEXPECT_EQ(ok, 3 * N + 1);      /* every ext2 program clean on real ext2 I/O     */
    KEXPECT(deep > flr);            /* deeper tiers reach edges direct writes never do */
}

/* Fair, paired guided-vs-random on the ext2 mode: for each meta-seed, a guided
 * and a random campaign with the same generator, budget K and starting
 * meta-seed. Asserted: only what holds robustly (all programs clean, real
 * coverage explored, a corpus forms). The guided-vs-random comparison is
 * MEASURED and printed, never asserted — it is reported as measured. */
KTEST(ufuzz, u4_ext2_guided_vs_random_paired) {
    UFUZZ_SKIP_IF_NOTIMING();
    const int K = 24;
    const unsigned META[3] = { 0xE2A1u, 0xE2B2u, 0xE2C3u };
    int prev = kfuzz_cov_enable(1);
    uint64_t gsum = 0, rsum = 0;
    int gwins = 0, gcorp = 0, clean = 1;
    for (int p = 0; p < 3; p++) {
        struct u4_campaign_result g = {0}, r = {0};
        u4_campaign(1, 1, META[p], K, &g);        /* ext2: guided */
        u4_campaign(1, 0, META[p], K, &r);        /* ext2: random (same meta-seed) */
        kprintf("u4-ext2 pair %d: guided bits=%llu corpus=%d | random bits=%llu\n",
                p, g.bits, g.corpus_n, r.bits);
        gsum += g.bits; rsum += r.bits; gcorp += g.corpus_n;
        if (g.bits > r.bits) gwins++;
        if (g.ok != K || r.ok != K) clean = 0;
    }
    kfuzz_cov_enable(prev);
    kprintf("u4-ext2 paired: guided total=%llu random total=%llu guided-wins=%d/3\n",
            gsum, rsum, gwins);
    KEXPECT(clean);                 /* every ext2 program clean in every campaign */
    KEXPECT(gsum > 0);              /* both explore real coverage                 */
    KEXPECT(rsum > 0);
    KEXPECT(gcorp > 0);             /* guided forms a corpus                      */
}

/* ---- Oracle canaries: a failed worker self-check must never read as OK ------
 * `/bin/ufuzz selfcheck <kind>` runs a program built to FAIL one specific
 * self-check (see ufuzz.c run_selfcheck). The controller must classify every one
 * as OUT_OTHER. If any comes back OUT_OK (0), that self-check is blind and every
 * "clean" result that depends on it is unproven. */
#define U4_OUT_OTHER 4

static int u4_run_selfcheck(unsigned kind) {
    char ks[16]; u4_utoa(kind, ks);
    char* argv[] = { "/bin/ufuzz", "selfcheck", ks, 0 };
    int pid = proc_spawn_user_argv("/bin/ufuzz", argv);
    if (pid <= 0) return -1;
    int st = -1; sys_waitpid(pid, &st);
    return st;
}

KTEST(ufuzz, selfcheck_failures_are_never_ok) {
    UFUZZ_SKIP_IF_NOTIMING();
    KEXPECT_EQ(u4_run_selfcheck(0), U4_OUT_OTHER);   /* expectation violated at call 0  */
    KEXPECT_EQ(u4_run_selfcheck(1), U4_OUT_OTHER);   /* expectation violated at call 1  */
    KEXPECT_EQ(u4_run_selfcheck(2), U4_OUT_OTHER);   /* out-buffers differ across replay */
    KEXPECT_EQ(u4_run_selfcheck(3), U4_OUT_OTHER);   /* program over the call capacity   */
}

/* Replay isolation: a program that reads the FS base and then changes it must
 * still replay identically — the executor restores the FS base between the two
 * passes. Hand-built (selfcheck kind 4), so it does not depend on which seeds
 * the generator happens to map to that shape. */
KTEST(ufuzz, replay_restores_fs_base) {
    UFUZZ_SKIP_IF_NOTIMING();
    KEXPECT_EQ(u4_run_selfcheck(4), 0);
}
