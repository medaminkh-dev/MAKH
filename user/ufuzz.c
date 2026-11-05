/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * /bin/ufuzz — UFUZZ Ring-3 Controller & Worker lifecycle (MAKH 1.4.0, U1).
 *
 * This is the unprivileged, Ring-3 half of UFUZZ (spec §2.2: the campaign
 * manager must NOT live in the kernel). It runs a small, fixed, deterministic
 * campaign that exercises every worker-lifecycle outcome the U1 acceptance
 * gates require, and proves the controller survives all of them:
 *
 *   TC0 NORMAL        a benign worker runs to completion            -> exit 0
 *   TC1 EXPECTED_ERR  a worker hits an expected -errno and handles it -> exit 0
 *   TC2 FAULT         a worker dereferences a bad pointer -> SIGSEGV (crash
 *                     containment: the controller must keep running)
 *   TC3 HANG          a worker spins forever -> the controller's watchdog
 *                     kills it and the hang is detected as a TIMEOUT
 *
 * Isolation (spec §3.2): each worker is a fresh fork(), so it has its own
 * address space — a fault or a wild write in a worker cannot corrupt the
 * controller. Budget enforcement: for each worker the controller also forks a
 * watchdog child that sleeps for the budget and then SIGKILLs the worker, so a
 * wedged worker can never hang the campaign (this is the liveness fix; remove
 * the watchdog and TC3 hangs the controller forever).
 *
 * The External Supervisor (spec §2.1) is tools/run_tests.py: it owns the QEMU
 * --timeout and the isa-debug-exit power-off, i.e. the authority to restart the
 * VM on a *kernel* hang. The controller only owns worker-level budgets.
 *
 * Result reporting reuses the MAKH worker convention (same as /bin/sigfuzz,
 * /bin/sigtest): a worker encodes its whole outcome in its exit status, which
 * the parent reads back with wait4. MAKH wait status: a value < 128 is a normal
 * exit code; a value >= 128 is 128 + terminating-signal. So 139 = 128+SIGSEGV
 * (fault) and 137 = 128+SIGKILL (killed by the watchdog).
 *
 * The controller exits 0 iff every testcase's observed outcome matched its
 * expected class — i.e. normal/expected-error workers ran clean, the faulting
 * worker was contained, and the hanging worker was detected. Any mismatch is a
 * diagnostic nonzero exit. kernel/tests/test_ufuzz.c reads that exit code.
 */
#include "usys.h"
#include "ufuzz_u3_programs.h"   /* U3: baked typed-program set (generated) */

/* usys.h exposes no direct exit wrapper (start.S exits with umain's return),
 * but a forked worker must exit without unwinding back into the controller. */
static void uexit(int code) { usyscall(SYS_EXIT, code, 0, 0); for (;;) {} }

/* ---- tiny console helpers (no libc in this brick) ------------------------ */
static unsigned ustrlen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void puts_(const char* s) { uwrite(1, s, ustrlen(s)); }
static void putu_(unsigned long v) {
    char b[24]; int i = 24; b[--i] = '\0';
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    uwrite(1, &b[i], (unsigned)(23 - i));
}

/* ---- timing -------------------------------------------------------------- */
static unsigned long now_ms(void) {
    struct timespec ts = {0, 0};
    uclock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

/* ---- worker lifecycle menu ----------------------------------------------
 * A U1 worker is NOT yet a generated program (that is U3); it is a fixed,
 * deterministic harness selecting one lifecycle path. It runs in the forked
 * child and never returns — it exits (or faults/hangs). */
enum { TC_NORMAL = 0, TC_EXPECTED_ERR, TC_FAULT, TC_HANG, TC_COUNT };

static void run_worker(int tc) {
    switch (tc) {
    case TC_NORMAL: {
        /* Benign, side-effect-light syscalls + a valid mapping round-trip. */
        (void)ugetpid();
        void* p = umap(4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        if ((long)p > 0) { *(volatile char*)p = 0x5A; umunmap(p, 4096); }
        uexit(0);
    }
    case TC_EXPECTED_ERR: {
        /* An invalid op must fail cleanly with a negative errno, not crash. */
        long r1 = uclose(-1);                 /* EBADF */
        long r2 = (long)uopen("/no/such/path/ufuzz", 0);   /* ENOENT */
        uexit((r1 < 0 && r2 < 0) ? 0 : 71);   /* 71 = unexpected success */
    }
    case TC_FAULT: {
        /* A wild write: the kernel must terminate *this* process with SIGSEGV,
         * leaving the controller untouched. Never returns. The address comes
         * through a volatile so the compiler can't prove it is near-zero and
         * warn (-Warray-bounds) on an intentional fault. */
        volatile unsigned long bad_addr = 0x1;
        *(volatile char*)bad_addr = 0x7F;
        uexit(72);                            /* unreachable if the fault fires */
    }
    case TC_HANG:
    default:
        for (;;) { }                          /* the watchdog must kill us */
    }
}

/* ---- U3: typed-program executor ----------------------------------------
 * Runs a baked typed program (ufuzz_u3_programs.h) in Ring 3 with runtime
 * resource tracking + success-gating, builds a per-call return trace, and (for
 * replay programs) runs it twice and requires an identical trace. A syscall
 * returning -errno is a recorded RESULT, not a worker failure; only a genuine
 * fault takes the process down (SIGSEGV), classified by the controller. */
#define U3_MAX_BUFS  8
#define U3_MAX_BUFSZ 128
#define U3_MAX_H     16
#define U3_MAX_CALLS 32

static unsigned char u3_arena[U3_MAX_BUFS][U3_MAX_BUFSZ];
static long u3_hval[U3_MAX_H];
static int  u3_hstate[U3_MAX_H];   /* 0 pending, 1 live, 2 failed */

/* Returns 0 on success, else a nonzero diagnostic code (what diverged). */
static int u3_run(const u3_program* pg, long* trace) {
    for (int b = 0; b < pg->nbufs && b < U3_MAX_BUFS; b++) {
        int n = pg->bufs[b].len; if (n > U3_MAX_BUFSZ) n = U3_MAX_BUFSZ;
        for (int k = 0; k < n; k++) u3_arena[b][k] = pg->bufs[b].data[k];
        for (int k = n; k < U3_MAX_BUFSZ; k++) u3_arena[b][k] = 0;
    }
    for (int h = 0; h < pg->nhandles && h < U3_MAX_H; h++) { u3_hval[h] = -1; u3_hstate[h] = 0; }

    for (int i = 0; i < pg->ncalls && i < U3_MAX_CALLS; i++) {
        const u3_call* c = &pg->calls[i];
        long a[4] = { 0, 0, 0, 0 };
        for (int k = 0; k < c->nargs && k < 4; k++) {
            const u3_arg* op = &c->args[k];
            switch (op->kind) {
            case U3_IMM:  a[k] = op->val; break;
            case U3_RES:  a[k] = u3_hval[op->val]; break;
            case U3_NIL:  a[k] = 0; break;
            case U3_BUF:  a[k] = (long)(unsigned long)u3_arena[op->val]; break;
            case U3_PATH: a[k] = (long)(unsigned long)pg->paths[op->val]; break;
            }
        }
        long ret = usyscall4(c->sysno, a[0], a[1], a[2], a[3]);
        /* Record a REPLAY-normalized result. MAKH's mmap is a bump allocator
         * that never reuses addresses (uvm_mmap advances mmap_cur; munmap does
         * not rewind it), so a successful mapping's address legitimately
         * differs run-to-run. Replay reproduces the observable OUTCOME (success
         * vs -errno), not the allocator-chosen address — the design's
         * reproducible-vs-non-deterministic distinction, made concrete. */
        trace[i] = (c->bind == U3_BIND_ADDR && ret > 0) ? 0 : ret;

        switch (c->expect_mode) {
        case U3_NONNEG: if (ret < 0)  return 0x100 | i; break;
        case U3_NEG:    if (ret >= 0) return 0x100 | i; break;
        case U3_EXACT:  if (ret != c->expect_val) return 0x100 | i; break;
        default: break;   /* U3_ANY */
        }

        if (c->bind == U3_BIND_FD) {
            if (ret >= 0) { u3_hval[c->bind_h0] = ret; u3_hstate[c->bind_h0] = 1; }
            else            u3_hstate[c->bind_h0] = 2;
        } else if (c->bind == U3_BIND_ADDR) {
            if (ret > 0)  { u3_hval[c->bind_h0] = ret; u3_hstate[c->bind_h0] = 1; }
            else            u3_hstate[c->bind_h0] = 2;
        } else if (c->bind == U3_BIND_PIPE) {
            if (ret == 0) {
                int* fds = (int*)(void*)u3_arena[c->bind_buf];
                u3_hval[c->bind_h0] = fds[0]; u3_hstate[c->bind_h0] = 1;
                u3_hval[c->bind_h1] = fds[1]; u3_hstate[c->bind_h1] = 1;
            } else { u3_hstate[c->bind_h0] = 2; u3_hstate[c->bind_h1] = 2; }
        }
    }

    if (pg->dc_len > 0) {                 /* data-check: dst == src for dc_len bytes */
        for (int k = 0; k < pg->dc_len; k++)
            if (u3_arena[pg->dc_dst][k] != u3_arena[pg->dc_src][k]) return 0x200 | k;
    }
    return 0;
}

static void u3_worker(int index) {
    const u3_program* pg = &U3_PROGRAMS[index];
    long t1[U3_MAX_CALLS], t2[U3_MAX_CALLS];
    int rc = u3_run(pg, t1);
    if (rc) uexit(rc);
    if (pg->replay) {
        int rc2 = u3_run(pg, t2);
        if (rc2) uexit(rc2);
        for (int i = 0; i < pg->ncalls && i < U3_MAX_CALLS; i++)
            if (t1[i] != t2[i]) uexit(0x300 | i);   /* non-deterministic replay */
    }
    uexit(0);
}

/* ---- outcome classification --------------------------------------------- */
enum { OUT_OK = 0, OUT_EXPECTED_ERR, OUT_FAULT, OUT_TIMEOUT, OUT_OTHER };

struct result { int outcome; int status; unsigned long ms; };

/* Budget per worker, in ms. A finite worker needs microseconds of CPU; this is
 * wall-clock, so it must be generous enough that even a heavily time-sliced
 * worker (e.g. 4 QEMUs sharing a slow CI runner) finishes well inside it —
 * otherwise the watchdog would SIGKILL a correct worker and it would be
 * misclassified as a hang. 2s gives large margin while still catching a true
 * infinite loop. */
#define BUDGET_MS 2000UL

/* The worker body is selected by kind so both campaigns share the same fork +
 * watchdog isolation: kind 0 = U1 lifecycle testcase, kind 1 = U3 program. The
 * selection is read from globals the child inherits across fork(). */
enum { WK_LIFECYCLE = 0, WK_U3 = 1 };
static int g_worker_kind, g_worker_arg;

static void worker_entry(void) {
    if (g_worker_kind == WK_U3) u3_worker(g_worker_arg);
    else                        run_worker(g_worker_arg);
    uexit(99);                                  /* unreachable: both uexit() */
}

/* Fork a worker (lifecycle testcase or U3 program), run it under the watchdog
 * budget, and classify the outcome. The controller never blocks indefinitely:
 * the watchdog guarantees the worker is reaped within ~BUDGET_MS even if it
 * wedges. */
static struct result run_one(int kind, int arg) {
    struct result r = { OUT_OTHER, -1, 0 };
    unsigned long t0 = now_ms();

    g_worker_kind = kind; g_worker_arg = arg;
    long wpid = ufork();
    if (wpid == 0) { worker_entry(); uexit(99); }   /* child: never returns */
    if (wpid < 0)  { r.outcome = OUT_OTHER; return r; }

    /* Watchdog: a second child that sleeps the budget then SIGKILLs the worker.
     * Removing this fork is the fail-first: TC_HANG then wedges the controller
     * in the wait below forever. */
    long wdpid = ufork();
    if (wdpid == 0) {
        usleep_ms(BUDGET_MS);
        ukill((int)wpid, SIGKILL);
        uexit(0);
    }

    int status = 0;
    uwaitpid((int)wpid, &status);                   /* returns when worker ends */
    r.ms = now_ms() - t0;

    /* Cancel + reap the watchdog (harmless if it already fired and exited). */
    if (wdpid > 0) { ukill((int)wdpid, SIGKILL); uwaitpid((int)wdpid, 0); }

    r.status = status;
    /* Signal-based classification is unambiguous and load-independent: the
     * watchdog is the ONLY source of SIGKILL (-> TIMEOUT), and a wild write is
     * the ONLY source of SIGSEGV (-> USER_FAULT). We do not use elapsed time to
     * classify (it drifts under load); ms is reported for information only. */
    int killed = status >= 128;
    int sig = killed ? status - 128 : 0;
    if      (sig == SIGSEGV) r.outcome = OUT_FAULT;
    else if (sig == SIGKILL) r.outcome = OUT_TIMEOUT;
    else if (killed)         r.outcome = OUT_OTHER;
    else if (status == 0)    r.outcome = OUT_OK;
    else                     r.outcome = OUT_OTHER;     /* nonzero exit: self-check fail */
    return r;
}

static const char* outcome_name(int o) {
    switch (o) {
    case OUT_OK:           return "OK";
    case OUT_EXPECTED_ERR: return "EXPECTED_ERR";
    case OUT_FAULT:        return "USER_FAULT";
    case OUT_TIMEOUT:      return "TIMEOUT";
    default:               return "OTHER";
    }
}

/* U1 campaign: the fixed worker-lifecycle self-check (NORMAL/EXPECTED_ERR/
 * FAULT/HANG). Returns 0 iff every outcome matched its expected class. */
static int run_lifecycle_campaign(void) {
    static const struct { int tc; int expect; const char* name; } plan[] = {
        { TC_NORMAL,       OUT_OK,      "NORMAL"       },
        { TC_EXPECTED_ERR, OUT_OK,      "EXPECTED_ERR" },
        { TC_FAULT,        OUT_FAULT,   "FAULT"        },
        { TC_HANG,         OUT_TIMEOUT, "HANG"         },
    };
    const int N = (int)(sizeof(plan) / sizeof(plan[0]));
    puts_("ufuzz: lifecycle campaign ("); putu_((unsigned long)N);
    puts_(" testcases, budget "); putu_(BUDGET_MS); puts_("ms)\n");

    int failures = 0, first = -1;
    for (int i = 0; i < N; i++) {
        struct result r = run_one(WK_LIFECYCLE, plan[i].tc);
        int ok = (r.outcome == plan[i].expect);
        if (!ok && first < 0) first = i;
        if (!ok) failures++;
        puts_("  tc="); putu_((unsigned long)i);
        puts_(" kind="); puts_(plan[i].name);
        puts_(" outcome="); puts_(outcome_name(r.outcome));
        puts_(" status="); putu_((unsigned long)(r.status < 0 ? 255 : r.status));
        puts_(" ms="); putu_(r.ms);
        puts_(ok ? " [match]\n" : " [MISMATCH]\n");
    }
    puts_("ufuzz: lifecycle done — "); putu_((unsigned long)(N - failures));
    puts_("/"); putu_((unsigned long)N);
    puts_(failures ? " matched (FAIL)\n" : " matched (PASS)\n");
    return failures ? (0x10 | (first & 0x0f)) : 0;
}

/* U3 campaign: run each baked typed program in an isolated, watchdog-bounded
 * worker. A program's expected outcome is FAULT if it is an expect_fault
 * program, else OK (ran to completion, every per-call expectation held, and —
 * for replay programs — its trace reproduced). Returns 0 iff all matched. */
static int run_u3_campaign(void) {
    const int N = U3_NPROGRAMS;
    puts_("ufuzz: U3 campaign ("); putu_((unsigned long)N);
    puts_(" typed programs, budget "); putu_(BUDGET_MS); puts_("ms)\n");

    int failures = 0, first = -1;
    for (int i = 0; i < N; i++) {
        int expect = U3_PROGRAMS[i].expect_fault ? OUT_FAULT : OUT_OK;
        struct result r = run_one(WK_U3, i);
        int ok = (r.outcome == expect);
        if (!ok && first < 0) first = i;
        if (!ok) failures++;
        puts_("  prog="); putu_((unsigned long)i);
        puts_(" name="); puts_(U3_PROGRAMS[i].name);
        puts_(" outcome="); puts_(outcome_name(r.outcome));
        puts_(" status="); putu_((unsigned long)(r.status < 0 ? 255 : r.status));
        puts_(ok ? " [match]\n" : " [MISMATCH]\n");
    }
    puts_("ufuzz: U3 done — "); putu_((unsigned long)(N - failures));
    puts_("/"); putu_((unsigned long)N);
    puts_(failures ? " matched (FAIL)\n" : " matched (PASS)\n");
    return failures ? (0x20 | (first & 0x0f)) : 0;
}

static int streq_u3(const char* s) {
    return s && s[0] == 'u' && s[1] == '3' && s[2] == '\0';
}

int umain(int argc, char** argv) {
    if (argc >= 2 && streq_u3(argv[1]))
        return run_u3_campaign();        /* `/bin/ufuzz u3` */
    return run_lifecycle_campaign();     /* default (U1) */
}
