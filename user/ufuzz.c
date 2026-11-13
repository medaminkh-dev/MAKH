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
#include "ufuzz_abi_errno.h"      /* U5: documented-errno table (generated) */

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
static void putl_(long v) {                     /* signed decimal */
    if (v < 0) { puts_("-"); putu_((unsigned long)(-(v + 1)) + 1UL); }
    else putu_((unsigned long)v);
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
#define U3_MAX_H     34      /* was 16: headroom for a deep fd-exhaustion stack (U4.2 EMFILE) */
#define U3_MAX_CALLS 72      /* was 32: headroom for ~30 open/close pairs (U4.2) */

/* The shared runtime generator (U4). Included AFTER the caps above so its
 * #ifndef-guarded mirrors no-op here and it uses these exact values. It stores
 * each call's syscall as an index (U3G_*); U3G_NR maps that to the real SYS_
 * number before u3_run executes the program. */
#include "ufuzz_u3_gen.h"
static long u3_contract_nr;  /* test seam: canary overrides the syscall whose contract is consulted */
static const long U3G_NR[U3G_NSYS] = {
    SYS_OPEN, SYS_READ, SYS_WRITE, SYS_CLOSE,
    SYS_MMAP, SYS_MPROTECT, SYS_MUNMAP, SYS_GETPID, SYS_ARCH_PRCTL,
    SYS_LSEEK, SYS_UNLINK,
    SYS_PIPE, SYS_PIPE2, SYS_DUP2, SYS_FCNTL, SYS_OPENAT, SYS_SOCKET
};

/* ---- U5.2: baseline-relative descriptor state ----------------------------
 * A worker does not assume which fds it starts with. Before pass 1 it records,
 * for every fd 0..31, the F_GETFL result (or -1 if closed), cross-checked against
 * fstat (the two probes must agree on open-vs-closed, else that disagreement is
 * itself a finding). The fds >= 3 that were FREE at baseline, in ascending order,
 * are what a U3_FDSLOT operand / U3_SLOT expectation ordinal k resolves to: with
 * lowest-free allocation, "the k-th baseline-free slot" is exactly the fd a
 * program's k-th outstanding allocation gets, whatever the worker inherited. */
#define U3_NFD 32                          /* kernel VFS_MAX_FDS */
#define U3_F_GETFL 3
static long u3_base_fl[U3_NFD];
static int  u3_free[U3_NFD], u3_nfree;
static unsigned char u3_stbuf[256];        /* fstat scratch (struct stat fits) */

static int u3_fdsnap(long* fl) {
    for (int fd = 0; fd < U3_NFD; fd++) {
        long r = usyscall(SYS_FCNTL, fd, U3_F_GETFL, 0);
        long s = usyscall(SYS_FSTAT, fd, (long)u3_stbuf, 0);
        if ((r >= 0) != (s == 0)) return 0x740 | fd;   /* probes disagree on fd */
        fl[fd] = (r >= 0) ? r : -1;
    }
    return 0;
}
/* Resolve a baseline-relative slot encoding: ordinal in the low 16 bits, any
 * higher bits ORed back (int-truncation cases). Returns 0 and sets *out, or a
 * 0x7FD diagnostic if the ordinal has no baseline-free slot. */
static int u3_slot(long enc, long* out) {
    long k = enc & 0xffff;
    if (k >= u3_nfree) return 0x7FD;
    *out = (long)u3_free[k] | (enc & ~0xffffL);
    return 0;
}

static unsigned char u3_arena[U3_MAX_BUFS][U3_MAX_BUFSZ];
static long u3_hval[U3_MAX_H];
static int  u3_hstate[U3_MAX_H];   /* 0 pending, 1 live, 2 failed */

/* Scratch for a U3_IOV operand: a struct iovec[] {void* base; size_t len} that
 * the executor fills with the RUNTIME addresses of arena buffers right before
 * the call. One is enough — readv/writev take a single iov arg. Layout matches
 * the kernel's struct iovec (two 8-byte words). */
static struct { unsigned long base, len; } u3_iov_scratch[U3_MAX_SEG];

/* Last expectation failure (U5.2): which syscall, what it returned, what the
 * contract required — printed by u3_fail so a NOT-CLEAN run is attributable from
 * the serial log alone. want_mode distinguishes the expectation kinds. */
static long u3_diag_nr, u3_diag_ret, u3_diag_want; static int u3_diag_mode = -1;
static long u3_diag_a[4];          /* U6: the call's resolved arguments (selector) */
/* U6: the per-call return trace of the pass in progress, so a failing worker
 * can print it (UFZ1 trace) — the artifact's syscall sequence + results. */
static long* u3_tcur; static int u3_tlen, u3_pass;

/* Returns 0 on success, else a nonzero diagnostic code (what diverged). */
static int u3_run(const u3_program* pg, long* trace) {
    /* Capacity guard (U4.2): a program larger than the executor's static arrays
     * must fail LOUDLY, never be silently truncated — a silent drop would lose
     * the tail balance closes, and the leaked fds would then be reaped at process
     * exit, hiding the bug from the U2 net-zero oracle. The generator is bounded
     * below these caps by construction (static_asserts in ufuzz_u3_gen.h); this
     * catches any slip. */
    if (pg->ncalls   > U3_MAX_CALLS) return 0x500;
    if (pg->nhandles > U3_MAX_H)     return 0x501;
    if (pg->nbufs    > U3_MAX_BUFS)  return 0x502;
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
            case U3_BADPTR: a[k] = op->val; break;         /* deliberate invalid pointer */
            case U3_FDSLOT: {
                int e = u3_slot(op->val, &a[k]);
                if (e) return e;
                break;
            }
            case U3_IOV: {
                /* Build the iovec array now, pointing each segment at the
                 * runtime address of its arena buffer (the host can't know it). */
                const u3_iov* iv = &pg->iovs[op->val];
                int ns = iv->nseg < U3_MAX_SEG ? iv->nseg : U3_MAX_SEG;
                for (int s = 0; s < ns; s++) {
                    u3_iov_scratch[s].base = (unsigned long)(void*)u3_arena[iv->seg_buf[s]];
                    u3_iov_scratch[s].len  = (unsigned long)iv->seg_len[s];
                }
                a[k] = (long)(unsigned long)&u3_iov_scratch[0];
                break;
            }
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
        u3_tlen = i + 1;
        u3_diag_nr = c->sysno; u3_diag_ret = ret; u3_diag_mode = c->expect_mode;
        for (int k = 0; k < 4; k++) u3_diag_a[k] = a[k];
        u3_diag_want = c->expect_val;
        if (c->expect_mode == U3_SLOT) (void)u3_slot(c->expect_val, &u3_diag_want);
        if (c->expect_mode == U3_BASEFL && c->expect_val >= 0 && c->expect_val < U3_NFD)
            u3_diag_want = u3_base_fl[c->expect_val];

        if (ret < 0 && ret >= -4095 && !u3_abi_errno_ok(u3_contract_nr ? u3_contract_nr : c->sysno, -ret))
            return 0x600 | i;   /* errno outside the ABI inventory's documented set */

        switch (c->expect_mode) {
        case U3_NONNEG: if (ret < 0)  return 0x100 | i; break;
        case U3_NEG:    if (ret >= 0) return 0x100 | i; break;
        case U3_EXACT:  if (ret != c->expect_val) return 0x100 | i; break;
        case U3_SLOT: {                                  /* exactly baseline slot k */
            long want; int e = u3_slot(c->expect_val, &want);
            if (e) return e;
            if (ret != want) return 0x100 | i;
            break;
        }
        case U3_BASEFL:                                  /* F_GETFL of baseline fd */
            if (c->expect_val < 0 || c->expect_val >= U3_NFD ||
                ret != u3_base_fl[c->expect_val]) return 0x100 | i;
            break;
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

    /* data-checks: each (dst, src, len) must match byte-for-byte. Multiple
     * checks let a scatter/gather program assert every segment reassembled. */
    for (int d = 0; d < pg->ndcs; d++) {
        const u3_dc* dc = &pg->dcs[d];
        for (int k = 0; k < dc->len; k++)
            if (u3_arena[dc->dst][k] != u3_arena[dc->src][k]) return 0x200 | (d << 5) | (k & 0x1f);
    }
    return 0;
}

/* FNV-1a over every program buffer (its declared length) — a fingerprint of
 * all OUT-buffer contents a run produced. Replay requires it to be identical
 * across the two runs: a syscall that returns the SAME value but writes
 * DIFFERENT bytes into an out buffer (non-deterministic output) is caught here,
 * which the per-call return trace alone would miss. */
static unsigned long u3_digest(const u3_program* pg) {
    unsigned long h = 1469598103934665603UL;        /* FNV offset basis */
    for (int b = 0; b < pg->nbufs && b < U3_MAX_BUFS; b++) {
        int n = pg->bufs[b].len; if (n > U3_MAX_BUFSZ) n = U3_MAX_BUFSZ;
        for (int k = 0; k < n; k++) { h ^= u3_arena[b][k]; h *= 1099511628211UL; }
    }
    return h;
}

/* Report a worker self-check failure. `diag` is the internal diagnostic:
 * 0x1NN expectation at call NN, 0x2NN data-check, 0x3NN replay divergence at
 * call NN, 0x400 out-buffer digest, 0x50N capacity. It must NOT be passed to
 * exit(): the kernel keeps only the low 8 bits of an exit code (and the
 * controller reads a status >= 128 as "killed by a signal"), so 0x100, 0x400 and
 * 0x500 all became exit code 0 — a failed self-check reported as OK. Print the
 * full diagnostic and exit with a nonzero class code in 1..127 instead. */
enum { U3X_EXPECT = 101, U3X_DATA = 102, U3X_REPLAY = 103, U3X_DIGEST = 104,
       U3X_CAPACITY = 105, U3X_UNKNOWN = 106, U3X_CONTRACT = 107,
       U3X_ISOLATION = 108 };
static void u3_fail(int diag) {
    int cls;
    const char* what;
    switch (diag >> 8) {
    case 1:  cls = U3X_EXPECT;   what = "expectation";  break;
    case 2:  cls = U3X_DATA;     what = "data-check";   break;
    case 3:  cls = U3X_REPLAY;   what = "replay";       break;
    case 4:  cls = U3X_DIGEST;   what = "digest";       break;
    case 5:  cls = U3X_CAPACITY; what = "capacity";     break;
    case 6:  cls = U3X_CONTRACT; what = "contract";     break;
    case 7:  cls = U3X_ISOLATION; what = "isolation";   break;
    default: cls = U3X_UNKNOWN;  what = "unknown";      break;
    }
    /* U6: machine-readable record of this failure, then the trace of the pass
     * that failed. Integers and fixed tokens only. */
    long pid = ugetpid();
    puts_("UFZ1 diag pid="); putl_(pid); puts_(" pass="); putl_(u3_pass);
    puts_(" cls="); putl_(cls); puts_(" what="); puts_(what);
    puts_(" diag="); putl_(diag); puts_(" call="); putl_(diag & 0xff);
    if ((diag >> 8) == 1 || (diag >> 8) == 6) {   /* expectation / contract */
        puts_(" nr="); putl_(u3_diag_nr); puts_(" emode="); putl_(u3_diag_mode);
        puts_(" ret="); putl_(u3_diag_ret); puts_(" want="); putl_(u3_diag_want);
        puts_(" a0="); putl_(u3_diag_a[0]); puts_(" a1="); putl_(u3_diag_a[1]);
    }
    puts_("\n");
    if (u3_tcur) {
        puts_("UFZ1 trace pid="); putl_(pid); puts_(" pass="); putl_(u3_pass);
        puts_(" n="); putl_(u3_tlen); puts_(" v=");
        for (int i = 0; i < u3_tlen; i++) { if (i) puts_(","); putl_(u3_tcur[i]); }
        puts_("\n");
    }
    puts_("  u3 self-check FAIL: "); puts_(what);
    puts_(" detail="); putu_((unsigned long)(diag & 0xff));
    if ((diag >> 8) == 1) {                     /* expectation: name the call */
        puts_(" nr="); putu_((unsigned long)u3_diag_nr);
        puts_(" mode="); putu_((unsigned long)u3_diag_mode);
        puts_(" ret="); if (u3_diag_ret < 0) { puts_("-"); putu_((unsigned long)-u3_diag_ret); }
                        else putu_((unsigned long)u3_diag_ret);
        puts_(" want="); if (u3_diag_want < 0) { puts_("-"); putu_((unsigned long)-u3_diag_want); }
                         else putu_((unsigned long)u3_diag_want);
    }
    puts_("\n");
    uexit(cls);
}

/* U7: when set (`... --sum <path>`), a clean worker writes the FNV-1a digest of
 * its pass-1 return trace and of its out-buffers to <path>, so a KTEST can
 * check that two execution paths ran the SAME program identically. */
static const char* g_sum_path;
static void u7_hex(char* o, unsigned long v) {
    for (int i = 15; i >= 0; i--) { o[i] = "0123456789abcdef"[v & 0xf]; v >>= 4; }
}
static void u7_write_sum(const u3_program* pg, const long* t, unsigned long bdig) {
    unsigned long h = 1469598103934665603UL;
    for (int i = 0; i < pg->ncalls && i < U3_MAX_CALLS; i++) {
        /* getpid's result is this worker's pid: the one return that legitimately
         * differs between two processes running the same program. */
        unsigned long v = (pg->calls[i].sysno == SYS_GETPID) ? 0 : (unsigned long)t[i];
        for (int k = 0; k < 8; k++) { h ^= (v >> (8 * k)) & 0xff; h *= 1099511628211UL; }
    }
    char line[34];
    u7_hex(line, h); line[16] = ' '; u7_hex(line + 17, bdig); line[33] = '\n';
    long fd = uopen(g_sum_path, 0x241);            /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd >= 0) { uwrite((int)fd, line, sizeof line); uclose((int)fd); }
}

static void u3_worker_pg(const u3_program* pg) {
    long t1[U3_MAX_CALLS], t2[U3_MAX_CALLS];
    /* Replay isolation: pass 2 must start from the process state pass 1 started
     * from. A program may change this thread's FS base (arch_prctl SET_FS); left
     * alone, a GET_FS issued before that SET_FS reads the original base on pass 1
     * and pass 1's leftover on pass 2. Save the base now, restore it between the
     * passes. (fds and mappings are released by the program itself; mmap
     * addresses are normalized in the trace.) */
    unsigned long fs0 = 0;
    usyscall(SYS_ARCH_PRCTL, U3G_ARCH_GET_FS, (long)&fs0, 0);
    /* fd baseline (U5.2): what this worker started with, NOT an assumed empty
     * table. Each pass must leave the fd state exactly as found (0x7NN after
     * pass 1, 0x7(80|NN) after pass 2, NN = the fd that differs). A program that
     * depends on a specific number of free slots declares it (need_free; exact
     * for programs that predict exhaustion) and fails loudly (0x7FE) otherwise. */
    long snap[U3_NFD];
    u3_tcur = 0; u3_pass = 0;
    int e = u3_fdsnap(u3_base_fl);
    if (e) u3_fail(e);
    u3_nfree = 0;
    for (int fd = 3; fd < U3_NFD; fd++) if (u3_base_fl[fd] < 0) u3_free[u3_nfree++] = fd;
    if (pg->need_free &&
        (pg->need_exact ? u3_nfree != pg->need_free : u3_nfree < pg->need_free))
        u3_fail(0x7FE);
    u3_tcur = t1; u3_tlen = 0; u3_pass = 1;
    int rc = u3_run(pg, t1);
    if (rc) u3_fail(rc);
    if ((e = u3_fdsnap(snap)) != 0) u3_fail(e);
    for (int fd = 0; fd < U3_NFD; fd++)
        if (snap[fd] != u3_base_fl[fd]) u3_fail(0x700 | fd);  /* pass 1 left fd state */
    unsigned long d1 = u3_digest(pg);               /* out-buffers after run 1 */
    if (pg->replay) {
        usyscall(SYS_ARCH_PRCTL, U3G_ARCH_SET_FS, (long)fs0, 0);
        u3_tcur = t2; u3_tlen = 0; u3_pass = 2;
        int rc2 = u3_run(pg, t2);
        if (rc2) u3_fail(rc2);
        if ((e = u3_fdsnap(snap)) != 0) u3_fail(e);
        for (int fd = 0; fd < U3_NFD; fd++)
            if (snap[fd] != u3_base_fl[fd]) u3_fail(0x780 | fd);
        for (int i = 0; i < pg->ncalls && i < U3_MAX_CALLS; i++)
            if (t1[i] != t2[i]) u3_fail(0x300 | i); /* non-deterministic return */
        if (u3_digest(pg) != d1) u3_fail(0x400);    /* non-deterministic out-buffers */
    }
    if (g_sum_path) u7_write_sum(pg, t1, d1);
    uexit(0);
}

static void u3_worker(int index) { u3_worker_pg(&U3_PROGRAMS[index]); }

/* U4: a worker that runs a freshly GENERATED program (built pre-fork and
 * inherited across the fork in g_gen_prog). */
static const u3_program* g_gen_prog;
static void u3gen_worker(void) { u3_worker_pg(g_gen_prog); }

/* ---- outcome classification --------------------------------------------- */
enum { OUT_OK = 0, OUT_EXPECTED_ERR, OUT_FAULT, OUT_TIMEOUT, OUT_OTHER };

struct result { int outcome; int status; unsigned long ms; long wpid; };

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
enum { WK_LIFECYCLE = 0, WK_U3 = 1, WK_U3GEN = 2 };
static int g_worker_kind, g_worker_arg;

static void worker_entry(void) {
    if      (g_worker_kind == WK_U3)    u3_worker(g_worker_arg);
    else if (g_worker_kind == WK_U3GEN) u3gen_worker();
    else                                run_worker(g_worker_arg);
    uexit(99);                                  /* unreachable: all uexit() */
}

/* Fork a worker (lifecycle testcase or U3 program), run it under the watchdog
 * budget, and classify the outcome. The controller never blocks indefinitely:
 * the watchdog guarantees the worker is reaped within ~BUDGET_MS even if it
 * wedges. */
static struct result run_one(int kind, int arg) {
    struct result r = { OUT_OTHER, -1, 0, -1 };
    unsigned long t0 = now_ms();

    g_worker_kind = kind; g_worker_arg = arg;
    long wpid = ufork();
    if (wpid == 0) { worker_entry(); uexit(99); }   /* child: never returns */
    if (wpid < 0)  { r.outcome = OUT_OTHER; return r; }
    r.wpid = wpid;

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
static int streq_cov(const char* s) {
    return s && s[0] == 'c' && s[1] == 'o' && s[2] == 'v' && s[3] == '\0';
}
static unsigned long parse_ul(const char* s) {
    unsigned long v = 0;
    if (s) for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (unsigned long)(*s - '0');
    return v;
}

/* U4: build ONE typed program from <seed> (shared generator), run it under the
 * watchdog, and exit with the outcome class (OUT_OK=0, OUT_FAULT=2,
 * OUT_TIMEOUT=3, else OTHER). A ring-0 driver spawns this per seed, reading
 * crashes/timeouts from the exit status and NEW coverage from the kernel map. */
/* mode 0 = skeleton (menu generator); 1 = ext2 block-map growth across the
 * direct/single/double-indirect tiers (U4.2); 2 = ext2 DIRECT-tier only (the
 * control the tier-milestone test measures against); 3 = fd-exhaustion boundary
 * (29 opens succeed, the 30th is -EMFILE); 4 = the same program expecting the
 * boundary one open too early — it MUST fail (negative control). */
/* U6: one machine-readable line per non-clean worker run, joining the worker
 * pid (UFZ1 diag/trace/ufault) to this controller's pid and its inputs. */
static void u6_res(const char* src, long a, long b, long c, struct result r) {
    if (r.outcome == OUT_OK) return;
    puts_("UFZ1 res src="); puts_(src);
    puts_(" cpid="); putl_(ugetpid()); puts_(" wpid="); putl_(r.wpid);
    puts_(" a="); putl_(a); puts_(" b="); putl_(b); puts_(" c="); putl_(c);
    puts_(" outcome="); puts_(outcome_name(r.outcome));
    puts_(" status="); putl_(r.status); puts_("\n");
}

static int run_cov_one(int mode, unsigned long base_seed, unsigned long mut_seed) {
    const u3_program* pg =
        (mode == 1) ? u3g_build_ext2(base_seed, mut_seed, 3) :
        (mode == 2) ? u3g_build_ext2(base_seed, mut_seed, 1) :
        (mode == 3) ? u3g_build_emfile(U3G_FD_BUDGET) :
        (mode == 4) ? u3g_build_emfile(U3G_FD_BUDGET - 1) :
        (mode == 5) ? u3g_build_fdops(base_seed, mut_seed) :
        (mode == 6) ? u3g_build_piperb() :
                      u3g_build(base_seed, mut_seed);
    for (int i = 0; i < pg->ncalls; i++)                 /* index -> real SYS_ */
        ((u3_call*)pg->calls)[i].sysno = U3G_NR[pg->calls[i].sysno];
    g_gen_prog = pg;
    struct result r = run_one(WK_U3GEN, 0);
    u6_res("cov", mode, (long)base_seed, (long)mut_seed, r);
    return r.outcome;
}

/* Oracle canaries: `/bin/ufuzz selfcheck <kind>` runs a tiny program BUILT TO FAIL
 * one specific worker self-check, through the normal worker path. It must
 * therefore never report OUT_OK — if it does, that self-check is blind.
 *   0  return-value expectation violated at call index 0
 *   1  return-value expectation violated at call index 1
 *   2  out-buffer bytes differ between the two replay passes (getrandom)
 *   3  program larger than the executor's call capacity
 * Kind 5: close(-1) returns -EBADF but the contract consulted is getpid's (no
 * errnos documented) -> the errno-set oracle must fail it. Kind 6 is the same
 * call with the real contract (EBADF documented) and must pass.
 * Kind 4 is the opposite, a POSITIVE control that must report OUT_OK: it reads
 * the FS base (GET_FS) and then changes it (SET_FS). Replay runs the program
 * twice in one process, so unless the executor restores the FS base between the
 * passes, pass 2 reads the value pass 1 left behind and the digest diverges.
 * U5.2 pairs (odd kind must FAIL, the following even kind must PASS):
 *   7 / 8   pipe(NULL) expected to succeed / expected -EFAULT (expectation oracle
 *           on a refused pointer argument);
 *   9 / 10  open() with no close / open()+close() (the fd-baseline isolation
 *           guard: a program that leaks an fd past a pass must be caught);
 *  11 / 12  need_free = 32 (more than the table can ever have free) / need_free
 *           = 1 (the baseline-precondition check).
 *  13       fcntl(fd, F_DUPFD, 32) = -EMFILE judged against fcntl's documented
 *           errno set: a POSITIVE control (the contract must list EMFILE). */
static int run_selfcheck(int kind) {
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;
    u3g_overflow = 0;
    u3g_rng r = { 1 };
    u3_call* c;
    long nr0 = SYS_GETPID;
    if (kind == 0) {
        c = u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
        c->expect_mode = U3_EXACT; c->expect_val = -1;          /* getpid never returns -1 */
    } else if (kind == 1) {
        (void)u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
        c = u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
        c->expect_mode = U3_EXACT; c->expect_val = -1;
    } else if (kind == 2) {
        int b = u3g_new_buf(&r, 16, 0);
        c = u3g_begin(U3G_GETPID, 3, U3_BIND_NONE, 0);          /* sysno set below */
        u3g_set(c, 0, U3_BUF, b); u3g_set(c, 1, U3_IMM, 16); u3g_set(c, 2, U3_IMM, 0);
        nr0 = SYS_GETRANDOM;
    } else if (kind == 5 || kind == 6) {
        c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_IMM, -1);                              /* close(-1) -> -EBADF */
        if (kind == 5) u3_contract_nr = SYS_GETPID;             /* getpid documents no errors */
    } else if (kind == 7 || kind == 8) {
        c = u3g_begin(U3G_PIPE, 1, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_BADPTR, 0);
        c->expect_mode = U3_EXACT; c->expect_val = (kind == 7) ? 0 : -U3G_EFAULT;
    } else if (kind == 9 || kind == 10) {
        u3g_emit_open();                                        /* expect: a new fd */
        if (kind == 10) u3g_emit_close(u3g_live_fd[0]);
    } else if (kind == 13) {
        u3g_emit_open();
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_RES, u3g_live_fd[0]); u3g_set(c, 1, U3_IMM, U3G_F_DUPFD);
        u3g_set(c, 2, U3_IMM, 32);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EMFILE;
        u3g_emit_close(u3g_live_fd[0]);
    } else if (kind == 11 || kind == 12) {
        (void)u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
    } else if (kind == 4) {
        int b = u3g_new_buf(&r, 8, 0);
        u3g_emit_arch_prctl(1, b);                              /* GET_FS -> buf */
        u3g_emit_arch_prctl(0, b);                              /* SET_FS        */
    } else {
        (void)u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
    }
    for (int i = 0; i < u3g_ncalls; i++)                        /* index -> real SYS_ */
        u3g_calls[i].sysno = U3G_NR[u3g_calls[i].sysno];
    if (kind == 2) u3g_calls[0].sysno = nr0;

    /* 9/10 are single-pass: with replay, the leaked fd would already change
     * pass 2's open() result and the trace check would catch it — the canary must
     * be visible ONLY to the fd-baseline guard. */
    u3g_prog.name = "selfcheck"; u3g_prog.expect_fault = 0;
    u3g_prog.replay = (kind == 9 || kind == 10) ? 0 : 1;
    u3g_prog.need_free = (kind == 11) ? 32 : (kind == 12) ? 1 : 0;
    u3g_prog.need_exact = 0;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 1;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    u3g_prog.calls = u3g_calls;
    u3g_prog.ncalls = (kind == 3) ? (U3_MAX_CALLS + 1) : u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
    g_gen_prog = &u3g_prog;
    struct result res = run_one(WK_U3GEN, 0);
    u6_res("selfcheck", kind, 0, 0, res);
    return res.outcome;
}


/* ---- U7: run a program from its textual IR -----------------------------------
 * `/bin/ufuzz run-ir <file> <k> [--sum <path>]` parses program <k> of a batch
 * file — the U3 IR with U5.3 `=expect` annotations and an optional `need` line,
 * programs separated by `### prog <k>` — into a u3_program and runs it through
 * the SAME worker path as `cov` (fd baseline, replay, digest, errno contract,
 * expectations). Only the syscalls the generator emits are accepted. Any parse
 * problem is a HARNESS failure (worker class 106, controller outcome OTHER),
 * never a fault. */
#define U7_FILE_MAX  (256 * 1024)
#define U7_MAX_PATHS 4
#define U7_PATH_MAX  64
static char u7_text[U7_FILE_MAX + 1];
static u3_call u7_calls[U3_MAX_CALLS];
static u3_buf u7_bufs[U3_MAX_BUFS];
static unsigned char u7_bufdata[U3_MAX_BUFS][U3_MAX_BUFSZ];
static char u7_pathdata[U7_MAX_PATHS][U7_PATH_MAX];
static const char* u7_paths[U7_MAX_PATHS];
static u3_program u7_prog;

static const struct { const char* name; long nr; } U7_SYS[] = {
    {"open", SYS_OPEN}, {"read", SYS_READ}, {"write", SYS_WRITE}, {"close", SYS_CLOSE},
    {"mmap", SYS_MMAP}, {"mprotect", SYS_MPROTECT}, {"munmap", SYS_MUNMAP},
    {"getpid", SYS_GETPID}, {"arch_prctl", SYS_ARCH_PRCTL}, {"lseek", SYS_LSEEK},
    {"unlink", SYS_UNLINK}, {"pipe", SYS_PIPE}, {"pipe2", SYS_PIPE2}, {"dup2", SYS_DUP2},
    {"fcntl", SYS_FCNTL}, {"openat", SYS_OPENAT}, {"socket", SYS_SOCKET},
};

static int u7_eq(const char* a, const char* b, int n) {   /* a[0..n) == b (NUL) */
    int i = 0;
    for (; i < n; i++) if (!b[i] || a[i] != b[i]) return 0;
    return b[i] == '\0';
}
/* Parse a signed decimal number; *p advances. Returns 0 on success. */
static int u7_num(const char** p, const char* end, long* out) {
    const char* s = *p; int neg = 0; unsigned long v = 0; int n = 0;
    if (s < end && *s == '-') { neg = 1; s++; }
    while (s < end && *s >= '0' && *s <= '9') {
        if (v > (~0UL - 9) / 10) return 1;
        v = v * 10 + (unsigned long)(*s - '0'); s++; n++;
    }
    if (!n) return 1;
    *out = neg ? -(long)v : (long)v;
    *p = s;
    return 0;
}
static int u7_hexbyte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Parse one program (the text in [s, e)). Returns 0 or a nonzero code. */
static int u7_parse(const char* s, const char* e) {
    int ncalls = 0, nbufs = 0, npaths = 0, nh = 0;
    u7_prog.need_free = 0; u7_prog.need_exact = 0;
    while (s < e) {
        const char* le = s;
        while (le < e && *le != '\n') le++;
        const char* t = s; const char* toks[16]; int tl[16]; int nt = 0;
        while (t < le && nt < 16) {
            while (t < le && *t == ' ') t++;
            if (t >= le) break;
            toks[nt] = t;
            while (t < le && *t != ' ') t++;
            tl[nt] = (int)(t - toks[nt]); nt++;
        }
        s = le + 1;
        if (nt == 0 || toks[0][0] == '#') continue;
        if (u7_eq(toks[0], "abi", tl[0]) || u7_eq(toks[0], "prng", tl[0])) continue;
        if (u7_eq(toks[0], "handles", tl[0])) {
            /* Count every word on the line (the token array keeps only 16). */
            int words = 0;
            for (const char* w = toks[0]; w < le; ) {
                while (w < le && *w == ' ') w++;
                if (w >= le) break;
                words++;
                while (w < le && *w != ' ') w++;
            }
            nh = (nt == 2 && tl[1] == 1 && toks[1][0] == '-') ? 0 : words - 1;
            if (nh > U3_MAX_H) return 0x10;
            continue;
        }
        if (u7_eq(toks[0], "need", tl[0])) {
            long v; const char* q = toks[1];
            if (nt != 3 || u7_num(&q, toks[1] + tl[1], &v) || v < 0 || v > 32) return 0x11;
            u7_prog.need_free = (int)v; u7_prog.need_exact = u7_eq(toks[2], "exact", tl[2]);
            continue;
        }
        if (u7_eq(toks[0], "path", tl[0])) {
            if (nt != 3 || npaths >= U7_MAX_PATHS || tl[2] >= U7_PATH_MAX) return 0x12;
            for (int k = 0; k < tl[2]; k++) u7_pathdata[npaths][k] = toks[2][k];
            u7_pathdata[npaths][tl[2]] = '\0';
            u7_paths[npaths] = u7_pathdata[npaths]; npaths++;
            continue;
        }
        if (u7_eq(toks[0], "buf", tl[0])) {
            long id, len; const char* q = toks[1];
            if (nt < 3 || nbufs >= U3_MAX_BUFS || u7_num(&q, toks[1] + tl[1], &id) || id != nbufs)
                return 0x13;
            q = toks[2];
            if (u7_num(&q, toks[2] + tl[2], &len) || len < 0 || len > U3_MAX_BUFSZ) return 0x14;
            for (int k = 0; k < len; k++) {
                int hi = -1, lo = -1;
                if (nt >= 4 && 2 * k + 1 < tl[3]) {
                    hi = u7_hexbyte(toks[3][2 * k]); lo = u7_hexbyte(toks[3][2 * k + 1]);
                }
                u7_bufdata[nbufs][k] = (hi < 0 || lo < 0) ? 0 : (unsigned char)(hi * 16 + lo);
            }
            u7_bufs[nbufs].data = u7_bufdata[nbufs]; u7_bufs[nbufs].len = (int)len; nbufs++;
            continue;
        }
        if (!u7_eq(toks[0], "call", tl[0])) return 0x15;
        if (ncalls >= U3_MAX_CALLS || nt < 2) return 0x16;
        u3_call* c = &u7_calls[ncalls++];
        c->sysno = -1;
        for (unsigned k = 0; k < sizeof U7_SYS / sizeof U7_SYS[0]; k++)
            if (u7_eq(toks[1], U7_SYS[k].name, tl[1])) c->sysno = U7_SYS[k].nr;
        if (c->sysno < 0) return 0x17;
        c->nargs = 0; c->bind = U3_BIND_NONE; c->bind_h0 = c->bind_h1 = c->bind_buf = 0;
        c->expect_mode = U3_ANY; c->expect_val = 0;
        for (int k = 0; k < 6; k++) { c->args[k].kind = U3_IMM; c->args[k].val = 0; }
        for (int i = 2; i < nt; i++) {
            const char* q = toks[i]; const char* qe = toks[i] + tl[i]; long v;
            if (tl[i] == 2 && q[0] == '-' && q[1] == '>') {        /* -> fd:h[,h] / addr:h */
                if (++i >= nt) return 0x18;
                q = toks[i]; qe = toks[i] + tl[i];
                int isfd = (qe - q > 3 && q[0] == 'f' && q[1] == 'd' && q[2] == ':');
                int isad = (qe - q > 5 && q[0] == 'a' && q[1] == 'd' && q[2] == 'd' && q[3] == 'r' && q[4] == ':');
                if (!isfd && !isad) return 0x19;
                q += isfd ? 3 : 5;
                if (u7_num(&q, qe, &v) || v < 0 || v >= nh) return 0x1a;
                c->bind_h0 = (int)v; c->bind = isfd ? U3_BIND_FD : U3_BIND_ADDR;
                if (isfd && q < qe && *q == ',') {
                    q++;
                    if (u7_num(&q, qe, &v) || v < 0 || v >= nh) return 0x1b;
                    c->bind_h1 = (int)v; c->bind = U3_BIND_PIPE;
                    c->bind_buf = -1;
                    for (int a = 0; a < c->nargs; a++)
                        if (c->args[a].kind == U3_BUF) c->bind_buf = (int)c->args[a].val;
                    if (c->bind_buf < 0) return 0x1c;
                }
                continue;
            }
            if (q[0] == '=') {                                    /* =kind[:v] */
                const char* k = q + 1; const char* ke = k;
                while (ke < qe && *ke != ':') ke++;
                long ev = 0;
                if (ke < qe) { const char* vq = ke + 1; if (u7_num(&vq, qe, &ev)) return 0x1d; }
                int kl = (int)(ke - k);
                if      (u7_eq(k, "any", kl))    c->expect_mode = U3_ANY;
                else if (u7_eq(k, "nonneg", kl)) c->expect_mode = U3_NONNEG;
                else if (u7_eq(k, "neg", kl))    c->expect_mode = U3_NEG;
                else if (u7_eq(k, "exact", kl))  c->expect_mode = U3_EXACT;
                else if (u7_eq(k, "slot", kl))   c->expect_mode = U3_SLOT;
                else if (u7_eq(k, "basefl", kl)) c->expect_mode = U3_BASEFL;
                else return 0x1e;
                c->expect_val = ev;
                continue;
            }
            if (c->nargs >= 4) return 0x1f;
            u3_arg* a = &c->args[c->nargs++];
            if (tl[i] == 4 && q[0] == 'n' && q[1] == 'u' && q[2] == 'l' && q[3] == 'l') {
                a->kind = U3_NIL; a->val = 0; continue;
            }
            const char* col = q;
            while (col < qe && *col != ':') col++;
            if (col >= qe) return 0x20;
            int kl = (int)(col - q);
            const char* vq = col + 1;
            if (u7_num(&vq, qe, &v) || vq != qe) return 0x21;
            if      (u7_eq(q, "imm", kl))  a->kind = U3_IMM;
            else if (u7_eq(q, "res", kl))  { a->kind = U3_RES;  if (v < 0 || v >= nh) return 0x22; }
            else if (u7_eq(q, "buf", kl))  { a->kind = U3_BUF;  if (v < 0 || v >= nbufs) return 0x23; }
            else if (u7_eq(q, "path", kl)) { a->kind = U3_PATH; if (v < 0 || v >= npaths) return 0x24; }
            else if (u7_eq(q, "slot", kl)) a->kind = U3_FDSLOT;
            else if (u7_eq(q, "bad", kl))  a->kind = U3_BADPTR;
            else return 0x25;
            a->val = v;
        }
    }
    if (ncalls == 0) return 0x26;
    u7_prog.name = "ir"; u7_prog.expect_fault = 0; u7_prog.replay = 1;
    u7_prog.nhandles = nh;
    u7_prog.bufs = u7_bufs; u7_prog.nbufs = nbufs;
    u7_prog.paths = u7_paths; u7_prog.npaths = npaths;
    u7_prog.iovs = 0; u7_prog.niovs = 0;
    u7_prog.calls = u7_calls; u7_prog.ncalls = ncalls;
    u7_prog.dcs = 0; u7_prog.ndcs = 0;
    return 0;
}

/* Locate program k (`### prog <k>` headers) in the batch file and parse it. */
static int u7_load(const char* file, unsigned long k) {
    long fd = uopen(file, 0);
    if (fd < 0) return 0x01;
    long n = 0, r;
    while (n < U7_FILE_MAX && (r = uread((int)fd, u7_text + n, (unsigned long)(U7_FILE_MAX - n))) > 0)
        n += r;
    uclose((int)fd);
    if (n >= U7_FILE_MAX) return 0x02;
    u7_text[n] = '\0';
    const char* s = u7_text; const char* e = u7_text + n;
    const char* start = 0; const char* stop = e;
    while (s < e) {
        const char* le = s;
        while (le < e && *le != '\n') le++;
        if (le - s > 9 && s[0] == '#' && s[1] == '#' && s[2] == '#' && s[3] == ' ' &&
            s[4] == 'p' && s[5] == 'r' && s[6] == 'o' && s[7] == 'g' && s[8] == ' ') {
            if (start) { stop = s; break; }
            const char* q = s + 9; long v;
            if (!u7_num(&q, le, &v) && (unsigned long)v == k) start = le + 1;
        }
        s = le + 1;
    }
    if (!start) return 0x03;
    return u7_parse(start, stop);
}

static int run_ir_one(const char* file, unsigned long k) {
    int e = u7_load(file, k);
    if (e) {
        /* A harness problem, reported in the same record format as a worker. */
        long pid = ugetpid();
        puts_("UFZ1 diag pid="); putl_(pid); puts_(" pass=0 cls=106 what=ir-parse diag=");
        putl_(e); puts_(" call=0\n");
        puts_("UFZ1 res src=ir cpid="); putl_(pid); puts_(" wpid="); putl_(pid);
        puts_(" a="); putl_((long)k); puts_(" b=0 c=0 outcome=OTHER status=106\n");
        return OUT_OTHER;
    }
    g_gen_prog = &u7_prog;
    struct result r = run_one(WK_U3GEN, 0);
    u6_res("ir", (long)k, 0, 0, r);
    return r.outcome;
}

static int streq_(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int umain(int argc, char** argv) {
    if (argc >= 3 && streq_(argv[1], "selfcheck"))
        return run_selfcheck((int)parse_ul(argv[2]));    /* `/bin/ufuzz selfcheck <kind>` */
    if (argc >= 2 && streq_u3(argv[1]))
        return run_u3_campaign();                        /* `/bin/ufuzz u3` */
    /* U7: an optional trailing `--sum <path>` makes a clean worker write its
     * trace/out-buffer digests there (equivalence checks). */
    if (argc >= 3 && streq_(argv[argc - 2], "--sum")) { g_sum_path = argv[argc - 1]; argc -= 2; }
    if (argc >= 4 && streq_(argv[1], "run-ir"))          /* `run-ir <file> <k>` */
        return run_ir_one(argv[2], parse_ul(argv[3]));
    if (argc >= 2 && streq_cov(argv[1]))                 /* `cov <mode> <base> <mut>` */
        return run_cov_one(argc >= 3 ? (int)parse_ul(argv[2]) : 0,
                           argc >= 4 ? parse_ul(argv[3]) : 0,
                           argc >= 5 ? parse_ul(argv[4]) : 0);
    return run_lifecycle_campaign();                     /* default (U1) */
}
