/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * MAKH UFUZZ U4 — shared typed-program generator (host + guest).
 *
 * PURE: builds a u3_program from a seed with NO syscalls, correct-by-
 * construction (mirrors the constraints of fuzz/u3/gen.py Gen.build — tracked
 * live fd/addr pools, operands that satisfy the U3 contract, everything
 * released at the end). It is compiled in TWO places from this one source:
 *
 *   - the guest, /bin/ufuzz `cov` mode: build a program, map the syscall INDEX
 *     in u3_call.sysno to a real SYS_ number, then run it through u3_run;
 *   - the host tool fuzz/u4/u4gen.c: build the same program and serialize it to
 *     the U3 IR text, which fuzz/u4/validate.py feeds to the Python U3 validator
 *     (the STRUCTURAL authority). Because both sides run THIS code, the Python
 *     validator checks the exact generator the guest uses — no QEMU, no serial
 *     capture, so the CI bridge is deterministic and fail-closed.
 *
 * The syscall is stored as an INDEX (U3G_*) so this header needs no SYS_
 * numbers: the guest resolves index -> SYS_ (U3G_NR in ufuzz.c), the host
 * resolves index -> name (U3G_NAME below) for the IR.
 */
#ifndef MAKHOS_UFUZZ_U3_GEN_H
#define MAKHOS_UFUZZ_U3_GEN_H

#include "ufuzz_u3_programs.h"   /* u3_program/u3_call/u3_arg/u3_buf + U3_* enums */

/* Executor capacity limits. These live in user/ufuzz.c as the ground truth;
 * mirrored here under #ifndef so this header compiles standalone on the host
 * (fuzz/u4/u4gen.c) yet never conflicts when ufuzz.c includes it after its own
 * definitions. Keep the values identical to ufuzz.c. */
#ifndef U3_MAX_BUFS
#define U3_MAX_BUFS  8
#endif
#ifndef U3_MAX_BUFSZ
#define U3_MAX_BUFSZ 128
#endif
#ifndef U3_MAX_H
#define U3_MAX_H     16
#endif

/* Generator syscall table. The index is what a generated u3_call.sysno holds. */
enum { U3G_OPEN, U3G_READ, U3G_WRITE, U3G_CLOSE,
       U3G_MMAP, U3G_MPROTECT, U3G_MUNMAP, U3G_GETPID, U3G_ARCH_PRCTL, U3G_NSYS };
static const char* const U3G_NAME[U3G_NSYS] = {
    "open", "read", "write", "close", "mmap", "mprotect", "munmap", "getpid",
    "arch_prctl"
};

#define U3G_STEPS     8
#define U3G_MAX_CALLS 24     /* steps + balance; < U3_MAX_CALLS (32) */

/* open() flags / mmap() args mirror fuzz/u3/gen.py (side-effect-free O_RDONLY
 * open of a file present in every boot's initrd; private anonymous mapping). */
#define U3G_O_RDONLY  0
#define U3G_PROT_RW   0x3
#define U3G_MAP_PA    0x22

/* arch_prctl selector-switched call (U4.1). These are the real `code` values the
 * host validator resolves by sel_value; a generated program emits them as the
 * `code` IMM, and the matching argp shape (SET_FS takes the base by value, so an
 * IMM; GET_FS writes to an 8-byte out buffer, so a BUF). ARCH_SET_FS is safe to
 * fuzz now that T-S01 rejects a non-canonical base with -EPERM; we also use a
 * canonical user-half value the worker never dereferences through %fs. */
#define U3G_ARCH_SET_FS  0x1002
#define U3G_ARCH_GET_FS  0x1003
#define U3G_FS_SAFE      0x1000          /* canonical, < USER_FS_BASE_END, never deref'd */

/* ---- scratch for ONE generated program (single-threaded use per TU) ------- */
static u3_call       u3g_calls[U3G_MAX_CALLS];
static u3_buf        u3g_bufs[U3_MAX_BUFS];
static unsigned char u3g_bufdata[U3_MAX_BUFS][U3_MAX_BUFSZ];
static const char* const u3g_paths[1] = { "/bin/hello" };
static const char*   u3g_handle_rtype[U3_MAX_H];   /* "fd" | "addr" per handle */
static u3_program    u3g_prog;

/* 64-bit LCG, identical constants to fuzz/u3/u3.py Lcg (not relied on for
 * cross-tool parity — only for per-tool determinism from a seed). */
typedef struct { unsigned long s; } u3g_rng;
static unsigned long u3g_next(u3g_rng* r) {
    r->s = r->s * 6364136223846793005UL + 1442695040888963407UL;
    return r->s >> 33;
}
static unsigned long u3g_below(u3g_rng* r, unsigned long n) {
    return n ? (u3g_next(r) % n) : 0;
}

/* ---- builder state -------------------------------------------------------- */
static int u3g_ncalls, u3g_nbufs, u3g_nhandles;
static int u3g_live_fd[U3_MAX_H],  u3g_nfd;
static int u3g_live_addr[U3_MAX_H], u3g_naddr;

static int u3g_new_handle(const char* rt) {
    int h = u3g_nhandles++;
    u3g_handle_rtype[h] = rt;
    return h;
}
static int u3g_new_buf(u3g_rng* r, int size, int fill) {
    int b = u3g_nbufs++;
    if (size > U3_MAX_BUFSZ) size = U3_MAX_BUFSZ;
    for (int i = 0; i < size; i++)
        u3g_bufdata[b][i] = fill ? (unsigned char)u3g_next(r) : 0;
    u3g_bufs[b].data = u3g_bufdata[b];
    u3g_bufs[b].len = size;
    return b;
}
static u3_call* u3g_begin(int sysno, int nargs, int bind, int h0) {
    u3_call* c = &u3g_calls[u3g_ncalls++];
    c->sysno = sysno; c->nargs = nargs;
    for (int i = 0; i < 6; i++) { c->args[i].kind = U3_IMM; c->args[i].val = 0; }
    c->bind = bind; c->bind_h0 = h0; c->bind_h1 = 0; c->bind_buf = 0;
    c->expect_mode = U3_ANY; c->expect_val = 0;   /* generated: outcome unknown */
    return c;
}
static void u3g_set(u3_call* c, int i, int kind, long val) {
    c->args[i].kind = kind; c->args[i].val = val;
}

static void u3g_emit_open(void) {
    int h = u3g_new_handle("fd");
    u3_call* c = u3g_begin(U3G_OPEN, 2, U3_BIND_FD, h);
    u3g_set(c, 0, U3_PATH, 0);            /* /bin/hello */
    u3g_set(c, 1, U3_IMM, U3G_O_RDONLY);
    u3g_live_fd[u3g_nfd++] = h;
}
static void u3g_emit_mmap(void) {
    int h = u3g_new_handle("addr");
    u3_call* c = u3g_begin(U3G_MMAP, 4, U3_BIND_ADDR, h);
    u3g_set(c, 0, U3_IMM, 0);
    u3g_set(c, 1, U3_IMM, 4096);
    u3g_set(c, 2, U3_IMM, U3G_PROT_RW);
    u3g_set(c, 3, U3_IMM, U3G_MAP_PA);
    u3g_live_addr[u3g_naddr++] = h;
}
static void u3g_emit_getpid(void) { (void)u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0); }

static void u3g_emit_write(u3g_rng* r) {
    int fd = u3g_live_fd[u3g_below(r, u3g_nfd)];
    int n = 1 + (int)u3g_below(r, 64);
    int b = u3g_new_buf(r, n, 1);
    u3_call* c = u3g_begin(U3G_WRITE, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd); u3g_set(c, 1, U3_BUF, b); u3g_set(c, 2, U3_IMM, n);
}
static void u3g_emit_read(u3g_rng* r) {
    int fd = u3g_live_fd[u3g_below(r, u3g_nfd)];
    int n = 1 + (int)u3g_below(r, 64);
    int b = u3g_new_buf(r, n, 0);
    u3_call* c = u3g_begin(U3G_READ, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd); u3g_set(c, 1, U3_BUF, b); u3g_set(c, 2, U3_IMM, n);
}
static void u3g_emit_close(int fd) {
    for (int i = 0; i < u3g_nfd; i++)                 /* drop from the live pool */
        if (u3g_live_fd[i] == fd) {
            for (int j = i; j < u3g_nfd - 1; j++) u3g_live_fd[j] = u3g_live_fd[j + 1];
            u3g_nfd--; break;
        }
    u3_call* c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd);
}
static void u3g_emit_mprotect(u3g_rng* r) {
    int a = u3g_live_addr[u3g_below(r, u3g_naddr)];
    u3_call* c = u3g_begin(U3G_MPROTECT, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, a); u3g_set(c, 1, U3_IMM, 4096); u3g_set(c, 2, U3_IMM, 0x1);
}
static void u3g_emit_munmap(int a) {
    for (int i = 0; i < u3g_naddr; i++)
        if (u3g_live_addr[i] == a) {
            for (int j = i; j < u3g_naddr - 1; j++) u3g_live_addr[j] = u3g_live_addr[j + 1];
            u3g_naddr--; break;
        }
    u3_call* c = u3g_begin(U3G_MUNMAP, 2, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, a); u3g_set(c, 1, U3_IMM, 4096);
}

/* arch_prctl with a mut-chosen selector (U4.1): SET_FS (code 0x1002, base by
 * value), GET_FS (0x1003, 8-byte out buffer), or an invalid code (the switch
 * default arm, -EINVAL). `buf` is a pre-reserved 8-byte buffer used by GET_FS.
 * All three selector choices are validity-preserving (the host validator
 * resolves the code and accepts the matching argp) and safe to run (SET_FS's
 * base is canonical user-half, T-S01-guarded, and the worker never derefs %fs).
 * Varying the selector walks do_arch_prctl's switch arms — that is the point. */
static void u3g_emit_arch_prctl(int selchoice, int buf) {
    u3_call* c = u3g_begin(U3G_ARCH_PRCTL, 2, U3_BIND_NONE, 0);
    if (selchoice == 0) {                       /* SET_FS: canonical base by value */
        u3g_set(c, 0, U3_IMM, U3G_ARCH_SET_FS);
        u3g_set(c, 1, U3_IMM, U3G_FS_SAFE);
    } else if (selchoice == 1) {                /* GET_FS: write base to out buffer */
        u3g_set(c, 0, U3_IMM, U3G_ARCH_GET_FS);
        u3g_set(c, 1, U3_BUF, buf);
    } else {                                    /* invalid code -> default arm      */
        u3g_set(c, 0, U3_IMM, 0x6666);
        u3g_set(c, 1, U3_BUF, buf);             /* argp unused by the default arm   */
    }
}

/* Build a structurally-valid, dependency-aware, balanced program. `base_seed`
 * fixes the SKELETON (which calls, their operands); a SEPARATE `mut_seed`-driven
 * PRNG picks only the SELECTORS of selector-switched calls (arch_prctl). So for
 * a fixed base_seed, varying mut_seed explores the same skeleton's selector
 * neighbourhood (the handler switch arms) rather than an unrelated program —
 * the structural locality seed-level mutation lacked (U4.1 / design §5).
 * Returns a pointer into static scratch (overwritten by the next call). */
static const u3_program* u3g_build(unsigned long base_seed, unsigned long mut_seed) {
    u3g_rng r = { base_seed };     /* structure */
    u3g_rng m = { mut_seed };      /* selectors / mutation */
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;

    for (int step = 0; step < U3G_STEPS; step++) {
        int menu[9], mm = 0;
        menu[mm++] = U3G_OPEN; menu[mm++] = U3G_MMAP; menu[mm++] = U3G_GETPID;
        menu[mm++] = U3G_ARCH_PRCTL;
        if (u3g_nfd > 0)   { menu[mm++] = U3G_WRITE; menu[mm++] = U3G_READ; menu[mm++] = U3G_CLOSE; }
        if (u3g_naddr > 0) { menu[mm++] = U3G_MPROTECT; menu[mm++] = U3G_MUNMAP; }
        int pick = menu[u3g_below(&r, mm)];

        /* Stay within executor/validator caps; otherwise fall back to getpid so
         * the choice is still consumed from the PRNG but adds no resource. */
        if (u3g_ncalls >= U3G_MAX_CALLS - 2) break;
        if ((pick == U3G_OPEN || pick == U3G_MMAP) && u3g_nhandles >= U3_MAX_H - 2) pick = U3G_GETPID;
        if ((pick == U3G_WRITE || pick == U3G_READ) && u3g_nbufs >= U3_MAX_BUFS)    pick = U3G_GETPID;
        if (pick == U3G_ARCH_PRCTL && u3g_nbufs >= U3_MAX_BUFS)                     pick = U3G_GETPID;

        switch (pick) {
        case U3G_OPEN:     u3g_emit_open();      break;
        case U3G_MMAP:     u3g_emit_mmap();      break;
        case U3G_WRITE:    u3g_emit_write(&r);   break;
        case U3G_READ:     u3g_emit_read(&r);    break;
        case U3G_CLOSE:    u3g_emit_close(u3g_live_fd[u3g_below(&r, u3g_nfd)]);   break;
        case U3G_MPROTECT: u3g_emit_mprotect(&r); break;
        case U3G_MUNMAP:   u3g_emit_munmap(u3g_live_addr[u3g_below(&r, u3g_naddr)]); break;
        case U3G_ARCH_PRCTL: {
            int b = u3g_new_buf(&r, 8, 0);             /* out buffer for GET_FS */
            u3g_emit_arch_prctl((int)u3g_below(&m, 3), b);   /* selector from mut */
            break;
        }
        default:           u3g_emit_getpid();    break;
        }
    }
    /* Balance: release everything still live (the U2 reset oracle relies on it). */
    while (u3g_nfd > 0)   u3g_emit_close(u3g_live_fd[u3g_nfd - 1]);
    while (u3g_naddr > 0) u3g_emit_munmap(u3g_live_addr[u3g_naddr - 1]);

    u3g_prog.name = "gen"; u3g_prog.expect_fault = 0; u3g_prog.replay = 1;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 1;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    u3g_prog.calls = u3g_calls; u3g_prog.ncalls = u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
    return &u3g_prog;
}

#endif /* MAKHOS_UFUZZ_U3_GEN_H */
