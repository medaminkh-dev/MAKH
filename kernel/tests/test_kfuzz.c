/**
 * MakhOS - test_kfuzz.c
 * CI smoke tests for the Phase 15 self-fuzzer. Each target runs a bounded,
 * deterministic campaign; the requirement is simple and strict: zero crashes,
 * zero oracle failures, and the heap/tree left consistent. The kfuzz core
 * itself (PRNG determinism, coverage feedback, sandbox recovery) is tested
 * directly too.
 */

#include <ktest.h>
#include <kfuzz.h>
#include <mm/kheap.h>

/* -------- core mechanics -------- */

KTEST(kfuzz, prng_is_deterministic_and_seed_sensitive) {
    kfuzz_rng_t a, b, c;
    kfuzz_rng_seed(&a, 0xABCDEF);
    kfuzz_rng_seed(&b, 0xABCDEF);
    kfuzz_rng_seed(&c, 0xABCDF0);
    int same = 1, diff = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t x = kfuzz_rand(&a), y = kfuzz_rand(&b), z = kfuzz_rand(&c);
        if (x != y) same = 0;
        if (x != z) diff = 1;
    }
    KEXPECT(same);          /* same seed -> same stream */
    KEXPECT(diff);          /* different seed -> different stream */
}

KTEST(kfuzz, rand_below_stays_in_range) {
    kfuzz_rng_t r; kfuzz_rng_seed(&r, 1);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = kfuzz_rand_below(&r, 7);
        KASSERT_TEST(v < 7);
    }
    KEXPECT_EQ(kfuzz_rand_below(&r, 0), 0u);   /* n==0 must not divide by zero */
}

KTEST(kfuzz, setjmp_longjmp_round_trips) {
    kfuzz_jmp_buf jb;
    volatile int stage = 0;
    int v = kfuzz_setjmp(jb);
    if (stage == 0) {
        stage = 1;
        KEXPECT_EQ(v, 0);                       /* first return */
        kfuzz_longjmp(jb, 42);
        KASSERT_TEST(0);                        /* unreachable */
    }
    KEXPECT_EQ(v, 42);                          /* came back via longjmp */
}

/* -------- the ring-0 sandbox catches and recovers from a real fault -------- */

KTEST(kfuzz, sandbox_recovers_from_page_fault) {
    /* t_fault deliberately dereferences an unmapped address. The sandbox must
     * catch the #PF, record it, and return control - not panic the kernel. */
    int rc = kfuzz_replay(KFUZZ_T_FAULT, 0x1, 1);
    KEXPECT_EQ(rc, 1);                          /* 1 = crash caught */

    /* And the kernel is fully alive afterwards: allocate, walk the heap, run a
     * clean campaign. If recovery had corrupted anything, these would fail. */
    void* p = kmalloc(128);
    KASSERT_TEST(p != NULL);
    kfree(p);
    KEXPECT_EQ(kheap_check(), 0);

    kfuzz_result_t res;
    KEXPECT_EQ(kfuzz_run(KFUZZ_T_HEAP, 50, 0xAAAA, &res), 0);
}

/* -------- each target, clean under a bounded campaign -------- */

static void run_target(uint32_t bit, const char* name) {
    (void)name;
    size_t heap0 = kheap_get_used();
    uint64_t bad0 = kheap_get_bad_frees();
    kfuzz_result_t res;
    int fails = kfuzz_run(bit, 400, 0xF00D5EED, &res);
    KEXPECT_EQ(fails, 0);
    KEXPECT_EQ(res.crashes, 0ull);
    KEXPECT_EQ(res.oracle_fails, 0ull);
    KEXPECT_EQ(kheap_check(), 0);
    KEXPECT_EQ(kheap_get_bad_frees(), bad0);
    KEXPECT(kheap_get_used() <= heap0 + 4096);   /* no target leaks */
}

KTEST(kfuzz, target_heap)    { run_target(KFUZZ_T_HEAP,    "heap");    }
KTEST(kfuzz, target_pmm)     { run_target(KFUZZ_T_PMM,     "pmm");     }
KTEST(kfuzz, target_string)  { run_target(KFUZZ_T_STRING,  "string");  }
KTEST(kfuzz, target_pthread) { run_target(KFUZZ_T_PTHREAD, "pthread"); }
KTEST(kfuzz, target_shell)   { run_target(KFUZZ_T_SHELL,   "shell");   }
KTEST(kfuzz, target_netrx)   { run_target(KFUZZ_T_NETRX,   "netrx");   }

KTEST(kfuzz, campaign_over_all_targets_builds_coverage) {
    kfuzz_cov_reset();
    kfuzz_result_t res;
    int fails = kfuzz_run(KFUZZ_T_ALL, 1500, 0x1234BEEF, &res);
    KEXPECT_EQ(fails, 0);
    KEXPECT(res.iterations >= 1500);
    KEXPECT(res.coverage > 100);                 /* the map actually filled in */
    KEXPECT(res.corpus > 0);                     /* some seeds found new edges */
}
