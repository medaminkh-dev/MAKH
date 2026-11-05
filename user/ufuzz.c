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
         * leaving the controller untouched. Never returns. */
        volatile char* bad = (volatile char*)0x1;
        *bad = 0x7F;
        uexit(72);                            /* unreachable if the fault fires */
    }
    case TC_HANG:
    default:
        for (;;) { }                          /* the watchdog must kill us */
    }
}

/* ---- outcome classification --------------------------------------------- */
enum { OUT_OK = 0, OUT_EXPECTED_ERR, OUT_FAULT, OUT_TIMEOUT, OUT_OTHER };

struct result { int outcome; int status; unsigned long ms; };

/* Budget per worker, in ms. Generous vs. the benign cases, short vs. a hang so
 * the campaign stays quick under slow TCG. */
#define BUDGET_MS 300UL

/* Fork a worker for testcase `tc`, run it under the watchdog budget, and
 * classify the outcome. The controller never blocks indefinitely: the watchdog
 * guarantees the worker is reaped within ~BUDGET_MS even if it wedges. */
static struct result run_one(int tc) {
    struct result r = { OUT_OTHER, -1, 0 };
    unsigned long t0 = now_ms();

    long wpid = ufork();
    if (wpid == 0) { run_worker(tc); uexit(99); }   /* child: never returns */
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
    int killed = status >= 128;
    int sig = killed ? status - 128 : 0;
    if (killed && r.ms >= (BUDGET_MS * 9) / 10) r.outcome = OUT_TIMEOUT;   /* budget elapsed */
    else if (killed && sig == SIGSEGV)          r.outcome = OUT_FAULT;
    else if (killed)                            r.outcome = OUT_OTHER;
    else if (status == 0)                       r.outcome = OUT_OK;
    else                                        r.outcome = OUT_OTHER;     /* self-check fail */
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

int umain(void) {
    /* Fixed campaign: one of each lifecycle class, in order. Deterministic. */
    static const struct { int tc; int expect; const char* name; } plan[] = {
        { TC_NORMAL,       OUT_OK,      "NORMAL"       },
        { TC_EXPECTED_ERR, OUT_OK,      "EXPECTED_ERR" },  /* worker self-verifies the errno -> exit 0 */
        { TC_FAULT,        OUT_FAULT,   "FAULT"        },
        { TC_HANG,         OUT_TIMEOUT, "HANG"         },
    };
    const int N = (int)(sizeof(plan) / sizeof(plan[0]));

    puts_("ufuzz: campaign start (");
    putu_((unsigned long)N);
    puts_(" testcases, budget ");
    putu_(BUDGET_MS);
    puts_("ms)\n");

    int failures = 0, first_fail_tc = -1;
    for (int i = 0; i < N; i++) {
        struct result r = run_one(plan[i].tc);
        int ok = (r.outcome == plan[i].expect);
        if (!ok && first_fail_tc < 0) first_fail_tc = i;
        if (!ok) failures++;

        /* One structured line per testcase (the U1 forerunner of a U6 artifact). */
        puts_("  tc="); putu_((unsigned long)i);
        puts_(" kind="); puts_(plan[i].name);
        puts_(" outcome="); puts_(outcome_name(r.outcome));
        puts_(" status="); putu_((unsigned long)(r.status < 0 ? 255 : r.status));
        puts_(" ms="); putu_(r.ms);
        puts_(ok ? " [match]\n" : " [MISMATCH]\n");
    }

    puts_("ufuzz: campaign done — ");
    putu_((unsigned long)(N - failures));
    puts_("/"); putu_((unsigned long)N);
    puts_(failures ? " matched (FAIL)\n" : " matched (PASS)\n");

    /* 0 = every lifecycle outcome observed and handled as expected. Reaching
     * this line at all means the controller survived the faulting and hanging
     * workers (crash + hang containment). A nonzero code flags which testcase
     * first diverged, for the KTEST and for triage. */
    if (failures) return 0x10 | (first_fail_tc & 0x0f);
    return 0;
}
