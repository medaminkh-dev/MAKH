/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - kfuzz.c
 * KFUZZ core: PRNG, coverage map, the ring-0 sandbox harness, and the
 * coverage-guided campaign loop. Targets live in kfuzz_targets.c.
 *
 * =============================================================================
 * How the sandbox works
 * =============================================================================
 * Fuzzing the kernel from inside the kernel means a bad input can fault in
 * ring 0 - normally fatal. KFUZZ makes faults survivable:
 *
 *   1. Before running a target it does kfuzz_setjmp() and raises a flag.
 *   2. If the target faults, the CPU exception handler (idt.c) sees the flag
 *      and calls kfuzz_report_fault(), which records seed + vector + address
 *      and kfuzz_longjmp()s straight back to the harness. The faulting call
 *      chain is abandoned; the campaign continues with the next seed.
 *   3. A watchdog thread catches hangs (a target that never returns), and
 *      after every target the invariant oracles (heap walker, process tree,
 *      rejected-free counter) must still hold.
 *
 * Because every fault is tagged with the exact seed that produced it,
 * `fuzz replay <seed> <target>` reproduces it deterministically.
 * =============================================================================
 */

#include <kfuzz.h>
#include <arch/idt.h>
#include <kernel.h>
#include <klog.h>
#include <sched.h>
#include <pthread.h>
#include <ktime.h>
#include <mm/kheap.h>
#include <lib/string.h>

/* From table.c / tree.c - invariant oracles. */
extern int proc_tree_check(void);

/* -------------------------------------------------------------------------- */
/* PRNG - xorshift64* : tiny, fast, and fully reproducible from a seed         */
/* -------------------------------------------------------------------------- */

void kfuzz_rng_seed(kfuzz_rng_t* r, uint64_t seed) {
    r->s = seed ? seed : 0x9E3779B97F4A7C15ull;   /* never zero */
}

uint64_t kfuzz_rand(kfuzz_rng_t* r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

uint32_t kfuzz_rand_below(kfuzz_rng_t* r, uint32_t n) {
    return n ? (uint32_t)(kfuzz_rand(r) % n) : 0;
}

void kfuzz_fill(kfuzz_rng_t* r, void* buf, size_t n) {
    uint8_t* p = buf;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)kfuzz_rand(r);
}

/* -------------------------------------------------------------------------- */
/* Coverage map - fed by the compiler via __sanitizer_cov_trace_pc            */
/* -------------------------------------------------------------------------- */
/*
 * Instrumented subject files (see the Makefile) call __sanitizer_cov_trace_pc
 * at every basic block. We fold the return address into a fixed bitmap and set
 * the bit. Setting a bit is idempotent, so the missing lock is harmless: a
 * rare lost update only under-counts coverage, never corrupts anything.
 */
#define COV_BITS   (1u << 16)                 /* 65536 edges */
#define COV_WORDS  (COV_BITS / 64)
static uint64_t cov_map[COV_WORDS];
static volatile int cov_enabled;              /* off until kfuzz starts */

void __sanitizer_cov_trace_pc(void);          /* silence -Wmissing-prototypes */
void __sanitizer_cov_trace_pc(void) {
    if (!cov_enabled) return;
    uint64_t pc = (uint64_t)(uintptr_t)__builtin_return_address(0);
    /* Mix the PC so nearby blocks land in different buckets. */
    uint64_t h = pc * 0x9E3779B97F4A7C15ull;
    uint32_t idx = (uint32_t)(h >> 48) & (COV_BITS - 1);
    cov_map[idx >> 6] |= 1ull << (idx & 63);
}

void kfuzz_cov_reset(void) {
    for (uint32_t i = 0; i < COV_WORDS; i++) cov_map[i] = 0;
}

static uint64_t popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (x * 0x0101010101010101ull) >> 56;
}

uint64_t kfuzz_cov_count(void) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < COV_WORDS; i++)
        if (cov_map[i]) n += popcount64(cov_map[i]);
    return n;
}

/* -------------------------------------------------------------------------- */
/* Sandbox state                                                              */
/* -------------------------------------------------------------------------- */

static struct {
    volatile int   active;         /* a target is running right now       */
    kfuzz_jmp_buf  jb;             /* where kfuzz_report_fault jumps back  */
    void         (*cleanup)(void); /* release locks held by the target    */
    volatile uint64_t seed;        /* seed feeding the current iteration   */
    const char*    target;         /* current target name                  */
    /* filled in by kfuzz_report_fault */
    volatile int   fault_vector;
    volatile uint64_t fault_rip;
    volatile uint64_t fault_addr;
    /* heartbeat + watchdog */
    volatile uint64_t heartbeat;   /* bumped each iteration                */
    volatile int   running;        /* campaign in progress (for watchdog)  */
} sb;

int kfuzz_in_sandbox(void) { return sb.active; }

void kfuzz_report_fault(void* regs_ptr) {
    registers_t* regs = (registers_t*)regs_ptr;
    sb.fault_vector = (int)regs->int_no;
    sb.fault_rip    = regs->rip;
    if (regs->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        sb.fault_addr = cr2;
    } else {
        sb.fault_addr = 0;
    }
    sb.active = 0;
    kfuzz_longjmp(sb.jb, sb.fault_vector + 1);   /* -> harness, nonzero */
}

/* -------------------------------------------------------------------------- */
/* Invariant oracles - run after every target                                 */
/* -------------------------------------------------------------------------- */

static uint64_t baseline_bad_frees;

static int oracles_ok(const char** why) {
    if (kheap_check() != 0)                     { *why = "heap corrupt";  return 0; }
    if (kheap_get_bad_frees() != baseline_bad_frees) { *why = "bad free"; return 0; }
    if (proc_tree_check() != 0)                 { *why = "proc tree";     return 0; }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Watchdog - catches a target that never returns                             */
/* -------------------------------------------------------------------------- */

#define KFUZZ_WATCHDOG_MS 4000

static void* watchdog_main(void* arg) {
    (void)arg;
    uint64_t last = sb.heartbeat;
    uint64_t last_change = clock_now_ms();
    while (sb.running) {
        sched_sleep_ms(200);
        if (sb.heartbeat != last) {
            last = sb.heartbeat;
            last_change = clock_now_ms();
            continue;
        }
        if (sb.active && clock_now_ms() - last_change > KFUZZ_WATCHDOG_MS) {
            /* The fuzzer thread is wedged inside a target (infinite loop or a
             * deadlock). Report the culprit seed loudly - this is a finding. */
            panic("KFUZZ watchdog: target '%s' hung on seed 0x%lx",
                  sb.target ? sb.target : "?", (unsigned long)sb.seed);
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* One sandboxed run of one target                                            */
/* -------------------------------------------------------------------------- */

/* Returns: 0 clean, 1 crash (caught fault), 2 oracle failure. */
static int run_one(const kfuzz_target_t* t, uint64_t seed, uint32_t iters,
                   kfuzz_result_t* res) {
    kfuzz_rng_t rng;
    kfuzz_rng_seed(&rng, seed);

    sb.seed    = seed;
    sb.target  = t->name;
    sb.cleanup = t->cleanup;

    int jumped = kfuzz_setjmp(sb.jb);
    if (jumped) {
        /* We got here via kfuzz_report_fault(): a CPU exception in the target.
         * Drop any lock it was holding, log the reproducer, and carry on. */
        if (sb.cleanup) sb.cleanup();
        sb.cleanup = NULL;
        if (res) {
            res->crashes++;
            res->last_crash_seed   = seed;
            res->last_crash_vector = sb.fault_vector;
        }
        KLOG_E("KFUZZ", "CRASH target=%s seed=0x%lx vector=%d rip=%p addr=%p"
                        "  replay: fuzz replay 0x%lx %s\n",
               t->name, (unsigned long)seed, sb.fault_vector,
               (void*)sb.fault_rip, (void*)sb.fault_addr,
               (unsigned long)seed, t->name);
        return 1;
    }

    sb.active = 1;
    t->fn(&rng, iters);
    sb.active = 0;
    sb.cleanup = NULL;

    const char* why = "";
    if (!oracles_ok(&why)) {
        if (res) res->oracle_fails++;
        KLOG_E("KFUZZ", "ORACLE FAIL target=%s seed=0x%lx (%s)  "
                        "replay: fuzz replay 0x%lx %s\n",
               t->name, (unsigned long)seed, why, (unsigned long)seed, t->name);
        /* Re-establish the baseline so one failure isn't reported forever. */
        baseline_bad_frees = kheap_get_bad_frees();
        return 2;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Campaign                                                                   */
/* -------------------------------------------------------------------------- */

#define KFUZZ_CORPUS_MAX  64

int kfuzz_run(uint32_t mask, uint64_t iters, uint64_t base_seed, kfuzz_result_t* out) {
    int ntargets;
    const kfuzz_target_t* targets = kfuzz_targets(&ntargets);

    kfuzz_result_t res;
    memset(&res, 0, sizeof(res));

    if (base_seed == 0) base_seed = clock_now_ms() * 0x100000001B3ull + 0xCAFED00D;
    baseline_bad_frees = kheap_get_bad_frees();

    /* Coverage-guided corpus: seeds that revealed new edges are kept and
     * mutated. A fresh random seed is tried the rest of the time. */
    uint64_t corpus[KFUZZ_CORPUS_MAX];
    int corpus_n = 0;
    kfuzz_rng_t meta;
    kfuzz_rng_seed(&meta, base_seed);

    const kfuzz_target_t* eligible[16];
    int neligible = 0;
    for (int i = 0; i < ntargets && neligible < 16; i++)
        if (mask & targets[i].bit) eligible[neligible++] = &targets[i];
    if (neligible == 0) {
        if (out) *out = res;
        return 0;
    }

    cov_enabled = 1;
    sb.running = 1;
    sb.heartbeat = 0;

    pthread_t wd;
    pthread_attr_t wa;
    pthread_attr_init(&wa);
    pthread_attr_setpriority_np(&wa, PRIO_DEFAULT - 4);   /* above the fuzzer */
    int have_wd = (pthread_create(&wd, &wa, watchdog_main, NULL) == 0);

    for (uint64_t i = 0; i < iters; i++) {
        sb.heartbeat = i + 1;

        /* Pick uniformly among the targets in the mask. (Rejection sampling
         * with a retry cap used to give up early on a single-target mask,
         * silently ending campaigns after a few dozen iterations.) */
        const kfuzz_target_t* t =
            eligible[kfuzz_rand_below(&meta, (uint32_t)neligible)];

        /* Seed: mutate a corpus entry, or draw fresh. */
        uint64_t seed;
        if (corpus_n > 0 && (kfuzz_rand(&meta) & 1)) {
            seed = corpus[kfuzz_rand_below(&meta, (uint32_t)corpus_n)]
                 ^ (kfuzz_rand(&meta) & 0xFFFF);
        } else {
            seed = kfuzz_rand(&meta);
        }

        uint64_t before = kfuzz_cov_count();
        run_one(t, seed, /*iters per run*/ 32, &res);
        uint64_t after = kfuzz_cov_count();

        if (after > before && corpus_n < KFUZZ_CORPUS_MAX)
            corpus[corpus_n++] = seed;         /* new coverage: keep the seed */

        res.iterations++;
    }

    sb.running = 0;
    if (have_wd) pthread_join(wd, NULL);

    res.coverage = kfuzz_cov_count();
    res.corpus   = (uint64_t)corpus_n;
    if (out) *out = res;
    return (int)(res.crashes + res.oracle_fails);
}

int kfuzz_replay(uint32_t target_bit, uint64_t seed, uint32_t iters) {
    int ntargets;
    const kfuzz_target_t* targets = kfuzz_targets(&ntargets);
    const kfuzz_target_t* t = NULL;
    for (int i = 0; i < ntargets; i++)
        if (targets[i].bit == target_bit) { t = &targets[i]; break; }
    if (!t) return -1;

    baseline_bad_frees = kheap_get_bad_frees();
    cov_enabled = 1;
    sb.running = 1;
    int rc = run_one(t, seed, iters, NULL);
    sb.running = 0;
    return rc;
}
