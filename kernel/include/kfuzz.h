/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - kfuzz.h
 * KFUZZ: an in-kernel, ring-0 coverage-guided fuzzer that attacks the kernel's
 * own subsystems from a sandboxed thread. See docs/PHASE15_KFUZZ.md.
 */

#ifndef MAKHOS_KFUZZ_H
#define MAKHOS_KFUZZ_H

#include <types.h>

/* Callee-saved regs + rsp + return address (see kernel/kfuzz/jmp.asm). */
typedef uint64_t kfuzz_jmp_buf[8];
int  kfuzz_setjmp(kfuzz_jmp_buf buf);
void kfuzz_longjmp(kfuzz_jmp_buf buf, int val) __attribute__((noreturn));

/* -------- reproducible PRNG (xorshift64*, per-run seed) -------- */
typedef struct kfuzz_rng { uint64_t s; } kfuzz_rng_t;
void     kfuzz_rng_seed(kfuzz_rng_t* r, uint64_t seed);
uint64_t kfuzz_rand(kfuzz_rng_t* r);
uint32_t kfuzz_rand_below(kfuzz_rng_t* r, uint32_t n);   /* [0, n)  */
void     kfuzz_fill(kfuzz_rng_t* r, void* buf, size_t n);

/* -------- targets -------- */
typedef enum {
    KFUZZ_T_HEAP   = 1u << 0,
    KFUZZ_T_PMM    = 1u << 1,
    KFUZZ_T_STRING = 1u << 2,
    KFUZZ_T_PTHREAD= 1u << 3,
    KFUZZ_T_SHELL  = 1u << 4,
    KFUZZ_T_NETRX  = 1u << 5,
    KFUZZ_T_VMSPACE= 1u << 7,   /* address spaces / COW / refcounts (Phase 17) */
    KFUZZ_T_VFS    = 1u << 8,   /* filesystem ops + tar parser (Phase 18)      */
    KFUZZ_T_TTY    = 1u << 9,   /* tty line discipline + signal masks (Phase 19) */
    KFUZZ_T_ELF    = 1u << 10,  /* ELF64 loader on malformed images (Phase 20-A) */
    KFUZZ_T_UVM    = 1u << 11,  /* anonymous mmap/munmap/brk/mprotect (Phase 20-B) */
    KFUZZ_T_PATH   = 1u << 12,  /* path canonicalisation (Phase 20-C)            */
    KFUZZ_T_ALL    = 0x3f | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) |
                     (1u << 11) | (1u << 12),
    KFUZZ_T_FAULT  = 1u << 6,   /* deliberate #PF; tests sandbox recovery */
} kfuzz_target_mask_t;

/* One fuzz target: run `iters` operations driven by `r`. Returns 0, or the
 * pid of a broken invariant / -1 on a detected inconsistency (a fault is
 * caught by the sandbox, not returned). */
typedef int (*kfuzz_target_fn)(kfuzz_rng_t* r, uint32_t iters);

typedef struct kfuzz_target {
    const char*     name;
    kfuzz_target_fn fn;
    uint32_t        bit;
    /* Called on sandbox recovery to drop any lock the target may hold. */
    void          (*cleanup)(void);
} kfuzz_target_t;

const kfuzz_target_t* kfuzz_targets(int* count);

/* -------- campaign -------- */
typedef struct kfuzz_result {
    uint64_t iterations;
    uint64_t crashes;          /* faults caught by the sandbox            */
    uint64_t oracle_fails;     /* invariant violations without a fault    */
    uint64_t coverage;         /* distinct coverage edges seen            */
    uint64_t corpus;           /* seeds kept for producing new coverage   */
    uint64_t last_crash_seed;  /* replay with `fuzz replay <seed> <tgt>`  */
    int      last_crash_vector;
} kfuzz_result_t;

/* Run `iters` iterations across the targets in `mask`, starting from
 * `base_seed` (0 => derive from the clock). Fills `out` if non-NULL.
 * Returns the number of crashes + oracle failures (0 = clean). */
int  kfuzz_run(uint32_t mask, uint64_t iters, uint64_t base_seed, kfuzz_result_t* out);

/* Deterministically re-run one target on one seed (crash replay). */
int  kfuzz_replay(uint32_t target_bit, uint64_t seed, uint32_t iters);

/* -------- coverage (fed by __sanitizer_cov_trace_pc) -------- */
void     kfuzz_cov_reset(void);
uint64_t kfuzz_cov_count(void);      /* number of edges hit so far */

/* -------- sandbox internals used by the exception handler -------- */
int  kfuzz_in_sandbox(void);         /* 1 while a target is running */
/* Record a CPU fault (regs is a registers_t*) and unwind to the harness. */
void kfuzz_report_fault(void* regs) __attribute__((noreturn));

#endif /* MAKHOS_KFUZZ_H */
