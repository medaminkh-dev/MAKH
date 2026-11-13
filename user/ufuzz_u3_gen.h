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
#define U3_MAX_H     34
#endif
#ifndef U3_MAX_CALLS
#define U3_MAX_CALLS 72
#endif

/* Generator syscall table. The index is what a generated u3_call.sysno holds.
 *
 * REPLAY-ISOLATION RULE for adding a call: replay runs a program twice in ONE
 * process, so any per-process state a call can change must be back to its
 * starting value before pass 2 — either released by the program itself (fds,
 * mappings, the ext2 file) or reset by the executor between the passes
 * (u3_worker_pg restores the FS base that arch_prctl SET_FS changes). State the
 * current calls can touch: fd table (closed by the program), mappings (unmapped;
 * addresses normalized in the trace), file offsets (die with the fd), the /mnt
 * file (unlinked), FS base (restored). A new call that touches anything else —
 * cwd, signal mask/handlers, brk, umask, GS base, rlimits — needs its reset added
 * to u3_worker_pg first. The digest + trace comparison is the backstop: leaked
 * state that changes any result or out-buffer shows up as a replay failure.
 * The fd table is checked directly (U5.2): u3_worker_pg snapshots every fd's
 * open state + F_GETFL before pass 1 and requires it unchanged after each pass,
 * so fd-surface calls (pipe/pipe2/dup2/fcntl/openat/socket) must release every
 * fd they create, must not dup2 onto an fd the worker inherited, and must not
 * F_SETFL a description shared with an inherited fd (the console). */
enum { U3G_OPEN, U3G_READ, U3G_WRITE, U3G_CLOSE,
       U3G_MMAP, U3G_MPROTECT, U3G_MUNMAP, U3G_GETPID, U3G_ARCH_PRCTL,
       U3G_LSEEK, U3G_UNLINK,
       U3G_PIPE, U3G_PIPE2, U3G_DUP2, U3G_FCNTL, U3G_OPENAT, U3G_SOCKET,
       U3G_NSYS };
static const char* const U3G_NAME[U3G_NSYS] = {
    "open", "read", "write", "close", "mmap", "mprotect", "munmap", "getpid",
    "arch_prctl", "lseek", "unlink",
    "pipe", "pipe2", "dup2", "fcntl", "openat", "socket"
};

#define U3G_STEPS     8
#define U3G_MAX_CALLS 70     /* steps + balance + deep sequences (U4.2); kept <= U3_MAX_CALLS */

/* Capacity invariants (U4.2): the generator must never emit a program the
 * executor would have to truncate. Enforced at compile time here, and at run
 * time by the u3g_begin capacity guard + the executor's 0x500 hard-fail. */
_Static_assert(U3G_MAX_CALLS <= U3_MAX_CALLS,
               "generator may emit more calls than the executor runs");
_Static_assert(U3G_STEPS * 2 <= U3G_MAX_CALLS,
               "skeleton worst case (each step plus its balance close) must fit");
_Static_assert(U3_MAX_H >= 4, "need handle headroom for deep fd-exhaustion stacks");

/* open() flags / mmap() args mirror fuzz/u3/gen.py (side-effect-free O_RDONLY
 * open of a file present in every boot's initrd; private anonymous mapping). */
#define U3G_O_RDONLY  0
#define U3G_PROT_RW   0x3
#define U3G_MAP_PA    0x22

/* ext2 block-map growth (U4.2 P1): a writable /mnt file opened create+truncate so
 * each run (and each replay pass) starts from an empty file. O_RDWR 2 | O_CREAT
 * 0x40 | O_TRUNC 0x200 — the Linux x86_64 values MAKH mirrors (user/usys.h). */
#define U3G_O_RW_CREAT_TRUNC  0x242
#define U3G_EXT2_PATH         1          /* index into u3g_paths: "/mnt/ufz" */

/* fd-exhaustion boundary (U4.2 P2). A process can hold U3G_FD_BUDGET file fds at
 * once: the kernel's VFS_MAX_FDS (32) minus the console fds 0..2, which fd_alloc
 * never hands out. kernel/tests/test_ufuzz.c static-asserts this against
 * VFS_MAX_FDS so the two cannot drift apart silently. */
#define U3G_FD_BUDGET  29
#define U3G_EMFILE     24
#define U3G_EBADF      9
#define U3G_EINVAL     22
_Static_assert(U3G_FD_BUDGET + 1 <= U3_MAX_H,
               "the fd-exhaustion program needs one handle per open");
_Static_assert(2 * (U3G_FD_BUDGET + 1) <= U3G_MAX_CALLS,
               "the fd-exhaustion program (opens + closes) must fit");

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
static const char* const u3g_paths[2] = { "/bin/hello", "/mnt/ufz" };
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
static int u3g_overflow;            /* set if any emission exceeded a capacity (U4.2) */

static int u3g_new_handle(const char* rt) {
    /* Capacity guard: never write past u3g_handle_rtype (see u3g_new_buf). */
    if (u3g_nhandles >= U3_MAX_H) { u3g_overflow = 1; return U3_MAX_H - 1; }
    int h = u3g_nhandles++;
    u3g_handle_rtype[h] = rt;
    return h;
}
static int u3g_new_buf(u3g_rng* r, int size, int fill) {
    /* Capacity guard (U4.2): never write past u3g_bufdata. A mode must not reserve
     * more than U3_MAX_BUFS buffers; flag an overflow so u3g_build makes it a loud
     * executor failure rather than a silent out-of-bounds write. */
    if (u3g_nbufs >= U3_MAX_BUFS) { u3g_overflow = 1; return U3_MAX_BUFS - 1; }
    int b = u3g_nbufs++;
    if (size > U3_MAX_BUFSZ) size = U3_MAX_BUFSZ;
    for (int i = 0; i < size; i++)
        u3g_bufdata[b][i] = fill ? (unsigned char)u3g_next(r) : 0;
    u3g_bufs[b].data = u3g_bufdata[b];
    u3g_bufs[b].len = size;
    return b;
}
static u3_call u3g_sink;    /* discard target if ever at capacity (guarded; see u3g_build) */
static u3_call* u3g_begin(int sysno, int nargs, int bind, int h0) {
    /* Capacity guard: never write past u3g_calls. If a mode ever tried to exceed
     * U3G_MAX_CALLS, flag it so u3g_build makes it a LOUD executor failure rather
     * than silently dropping the overflow call. */
    u3_call* c;
    if (u3g_ncalls < U3G_MAX_CALLS) c = &u3g_calls[u3g_ncalls++];
    else { u3g_overflow = 1; c = &u3g_sink; }
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
    c->expect_mode = U3_NONNEG;           /* existing file, table not full: a new fd */
    u3g_live_fd[u3g_nfd++] = h;
}
static void u3g_emit_mmap(void) {
    int h = u3g_new_handle("addr");
    u3_call* c = u3g_begin(U3G_MMAP, 4, U3_BIND_ADDR, h);
    u3g_set(c, 0, U3_IMM, 0);
    u3g_set(c, 1, U3_IMM, 4096);
    u3g_set(c, 2, U3_IMM, U3G_PROT_RW);
    u3g_set(c, 3, U3_IMM, U3G_MAP_PA);
    c->expect_mode = U3_NONNEG;           /* one anonymous page: a user address */
    u3g_live_addr[u3g_naddr++] = h;
}
static void u3g_emit_getpid(void) {
    u3_call* c = u3g_begin(U3G_GETPID, 0, U3_BIND_NONE, 0);
    c->expect_mode = U3_NONNEG;           /* the caller's pid */
}

static void u3g_emit_write(u3g_rng* r) {
    int fd = u3g_live_fd[u3g_below(r, u3g_nfd)];
    int n = 1 + (int)u3g_below(r, 64);
    int b = u3g_new_buf(r, n, 1);
    u3_call* c = u3g_begin(U3G_WRITE, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd); u3g_set(c, 1, U3_BUF, b); u3g_set(c, 2, U3_IMM, n);
    /* Every skeleton fd is opened O_RDONLY (u3g_emit_open), so the contract says
     * this write MUST be refused with -EBADF. A kernel that lets it through — the
     * T-V01 defect — fails this expectation instead of passing silently. */
    c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
}
static void u3g_emit_read(u3g_rng* r) {
    int fd = u3g_live_fd[u3g_below(r, u3g_nfd)];
    int n = 1 + (int)u3g_below(r, 64);
    int b = u3g_new_buf(r, n, 0);
    u3_call* c = u3g_begin(U3G_READ, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd); u3g_set(c, 1, U3_BUF, b); u3g_set(c, 2, U3_IMM, n);
    c->expect_mode = U3_NONNEG;           /* readable fd: bytes read, 0 at EOF */
}
static void u3g_emit_close(int fd) {
    for (int i = 0; i < u3g_nfd; i++)                 /* drop from the live pool */
        if (u3g_live_fd[i] == fd) {
            for (int j = i; j < u3g_nfd - 1; j++) u3g_live_fd[j] = u3g_live_fd[j + 1];
            u3g_nfd--; break;
        }
    u3_call* c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, fd);
    c->expect_mode = U3_EXACT; c->expect_val = 0;     /* a live fd closes cleanly */
}
static void u3g_emit_mprotect(u3g_rng* r) {
    int a = u3g_live_addr[u3g_below(r, u3g_naddr)];
    u3_call* c = u3g_begin(U3G_MPROTECT, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, a); u3g_set(c, 1, U3_IMM, 4096); u3g_set(c, 2, U3_IMM, 0x1);
    c->expect_mode = U3_EXACT; c->expect_val = 0;     /* a live mapping */
}
static void u3g_emit_munmap(int a) {
    for (int i = 0; i < u3g_naddr; i++)
        if (u3g_live_addr[i] == a) {
            for (int j = i; j < u3g_naddr - 1; j++) u3g_live_addr[j] = u3g_live_addr[j + 1];
            u3g_naddr--; break;
        }
    u3_call* c = u3g_begin(U3G_MUNMAP, 2, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, a); u3g_set(c, 1, U3_IMM, 4096);
    c->expect_mode = U3_EXACT; c->expect_val = 0;     /* a live mapping */
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
        c->expect_mode = U3_EXACT; c->expect_val = 0;
    } else if (selchoice == 1) {                /* GET_FS: write base to out buffer */
        u3g_set(c, 0, U3_IMM, U3G_ARCH_GET_FS);
        u3g_set(c, 1, U3_BUF, buf);
        c->expect_mode = U3_EXACT; c->expect_val = 0;
    } else {                                    /* invalid code -> default arm      */
        u3g_set(c, 0, U3_IMM, 0x6666);
        u3g_set(c, 1, U3_BUF, buf);             /* argp unused by the default arm   */
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EINVAL;
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
    u3g_overflow = 0;

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
    u3g_prog.need_free = 0; u3g_prog.need_exact = 0;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 1;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    /* If any emission overflowed capacity, report an over-cap ncalls so the
     * executor's 0x500 guard rejects the program loudly instead of running a
     * silently-truncated one. By construction (static_asserts + per-mode depth
     * caps) this never fires. */
    u3g_prog.calls = u3g_calls;
    u3g_prog.ncalls = u3g_overflow ? (U3_MAX_CALLS + 1) : u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
    return &u3g_prog;
}

/* ext2 block-map growth (U4.2 P1). A single writable /mnt file is grown by a
 * sequence of positioned writes whose OFFSETS (the mut-driven mutation
 * dimension) span the ext2 block-map tiers: direct (<12 KiB), single-indirect
 * (>=12 KiB), and toward double-indirect — so the generated traffic walks
 * alloc_file_block / alloc_indirect / alloc_block across tiers (kernel/fs/ext2.c).
 * base_seed fixes the skeleton (how many writes); mut_seed picks the offsets, so
 * the guided campaign keeps a fruitful skeleton and mutates offsets to reach
 * adjacent tiers. The file is O_TRUNC'd on open and unlinked at the end, so every
 * run — and every in-process replay pass — starts from an empty file, which is
 * what keeps the result deterministic under the harness's snapshot=on disk. */
/* Exact tier byte ranges for the test image's 1 KiB blocks (per = 1024/4 = 256
 * block pointers per indirect block): direct = file blocks 0..11, single-indirect
 * = 12..267, double-indirect = 268.. . A write is at most 32 bytes, so each range
 * stops 32 bytes short of the next tier — a "direct" write never spills into
 * block 12. `tiers` = 1 keeps every write in the direct tier (the CONTROL mode the
 * tier-milestone test compares against); 3 spans direct/single/double. */
#define U3G_T_SINGLE  (12UL * 1024)              /* first single-indirect byte  */
#define U3G_T_DOUBLE  ((12UL + 256UL) * 1024)    /* first double-indirect byte  */
static unsigned long u3g_ext2_offset(u3g_rng* m, int tiers) {
    unsigned long pick = u3g_below(m, 10);
    if (tiers <= 1 || pick < 4)
        return u3g_below(m, U3G_T_SINGLE - 32);                          /* direct */
    if (pick < 8)
        return U3G_T_SINGLE + u3g_below(m, U3G_T_DOUBLE - U3G_T_SINGLE - 32); /* single */
    return U3G_T_DOUBLE + u3g_below(m, 64UL * 1024);                     /* double */
}
static const u3_program* u3g_build_ext2(unsigned long base_seed, unsigned long mut_seed,
                                        int tiers) {
    u3g_rng r = { base_seed };   /* structure: how many writes */
    u3g_rng m = { mut_seed };    /* offsets: which tiers (the mutation dimension) */
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;
    u3g_overflow = 0;

    int h = u3g_new_handle("fd");
    u3_call* c = u3g_begin(U3G_OPEN, 2, U3_BIND_FD, h);
    u3g_set(c, 0, U3_PATH, U3G_EXT2_PATH);
    u3g_set(c, 1, U3_IMM, U3G_O_RW_CREAT_TRUNC);
    c->expect_mode = U3_NONNEG;
    u3g_live_fd[u3g_nfd++] = h;

    /* Reference model of the file: after O_TRUNC it is empty, and its size is the
     * furthest byte any write reached. That is enough to state the exact result
     * of every call below. */
    unsigned long fsize = 0;
    int nwrites = 2 + (int)u3g_below(&r, 5);        /* 2..6 positioned writes */
    for (int w = 0; w < nwrites; w++) {
        unsigned long off = u3g_ext2_offset(&m, tiers);
        c = u3g_begin(U3G_LSEEK, 3, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_RES, h); u3g_set(c, 1, U3_IMM, (long)off); u3g_set(c, 2, U3_IMM, 0);
        c->expect_mode = U3_EXACT; c->expect_val = (long)off;   /* the new offset */
        int n = 1 + (int)u3g_below(&r, 32);
        int b = u3g_new_buf(&r, n, 1);
        c = u3g_begin(U3G_WRITE, 3, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_RES, h); u3g_set(c, 1, U3_BUF, b); u3g_set(c, 2, U3_IMM, n);
        c->expect_mode = U3_EXACT; c->expect_val = n;           /* all n bytes written */
        if (off + (unsigned long)n > fsize) fsize = off + (unsigned long)n;
    }
    /* read a block back from offset 0 (exercises the read block-map walk and feeds
     * the out-buffer digest oracle). */
    c = u3g_begin(U3G_LSEEK, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, h); u3g_set(c, 1, U3_IMM, 0); u3g_set(c, 2, U3_IMM, 0);
    c->expect_mode = U3_EXACT; c->expect_val = 0;
    int rb = u3g_new_buf(&r, 32, 0);
    c = u3g_begin(U3G_READ, 3, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_RES, h); u3g_set(c, 1, U3_BUF, rb); u3g_set(c, 2, U3_IMM, 32);
    c->expect_mode = U3_EXACT;                       /* 32 bytes, or the whole file */
    c->expect_val  = (fsize < 32) ? (long)fsize : 32;

    u3g_emit_close(h);                               /* close the fd (balance) */
    c = u3g_begin(U3G_UNLINK, 1, U3_BIND_NONE, 0);   /* remove the file (free tiers) */
    u3g_set(c, 0, U3_PATH, U3G_EXT2_PATH);
    c->expect_mode = U3_EXACT; c->expect_val = 0;

    u3g_prog.name = "gen_ext2"; u3g_prog.expect_fault = 0; u3g_prog.replay = 1;
    u3g_prog.need_free = 0; u3g_prog.need_exact = 0;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 2;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    u3g_prog.calls = u3g_calls;
    u3g_prog.ncalls = u3g_overflow ? (U3_MAX_CALLS + 1) : u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
    return &u3g_prog;
}

/* fd-exhaustion boundary (U4.2 P2) — a SELF-CHECKING capacity test, not a
 * coverage experiment. Open `ok_opens` + 1 files without closing any: the first
 * `ok_opens` must succeed and the last must fail with exactly -EMFILE. Then close
 * every handle: each successful open's close must return exactly 0 — which also
 * proves the fds were DISTINCT (had the kernel handed out one slot twice, the
 * second close of it would return -EBADF) — and closing the failed handle (fd -1)
 * must return exactly -EBADF. The expectations are checked by the executor, so
 * the program fails loudly if the boundary is anywhere but `ok_opens`.
 * Called with U3G_FD_BUDGET it must pass; with U3G_FD_BUDGET - 1 it expects
 * -EMFILE one open too early and MUST fail (the test's negative control).
 * Opens are O_RDONLY of an initrd file: no filesystem side effect. */
static const u3_program* u3g_build_emfile(int ok_opens) {
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;
    u3g_overflow = 0;
    int total = ok_opens + 1;

    for (int i = 0; i < total; i++) {
        int h = u3g_new_handle("fd");
        u3_call* c = u3g_begin(U3G_OPEN, 2, U3_BIND_FD, h);
        u3g_set(c, 0, U3_PATH, 0);                    /* /bin/hello */
        u3g_set(c, 1, U3_IMM, U3G_O_RDONLY);
        if (i < ok_opens) { c->expect_mode = U3_NONNEG; }
        else              { c->expect_mode = U3_EXACT; c->expect_val = -U3G_EMFILE; }
    }
    for (int i = 0; i < total; i++) {
        u3_call* c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_RES, i);
        c->expect_mode = U3_EXACT;
        c->expect_val  = (i < ok_opens) ? 0 : -U3G_EBADF;
    }

    u3g_prog.name = "gen_emfile"; u3g_prog.expect_fault = 0; u3g_prog.replay = 1;
    /* The boundary is only meaningful if exactly U3G_FD_BUDGET slots are free at
     * baseline; the executor checks that precondition instead of assuming it. */
    u3g_prog.need_free = U3G_FD_BUDGET; u3g_prog.need_exact = 1;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 1;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    u3g_prog.calls = u3g_calls;
    u3g_prog.ncalls = u3g_overflow ? (U3_MAX_CALLS + 1) : u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
    return &u3g_prog;
}

/* ===========================================================================
 * U5.2 — fd-surface model (modes 5 and 6)
 *
 * Every call that allocates or releases a descriptor (open, openat, pipe, pipe2,
 * dup2, fcntl F_DUPFD, socket, close — and dup2's implicit close of its target)
 * goes through ONE model of the fd table, so every expected result is computed,
 * never U3_ANY. The model works in baseline ORDINALS: ordinal k is the k-th fd
 * >= 3 that was free when the worker started (the executor resolves it, see
 * u3_slot in ufuzz.c). Allocation is lowest-free, and the program never touches
 * an fd it did not create, so "lowest free fd >= slot(k)" is always
 * slot(lowest free ordinal >= k).
 *
 * Contract encoded here (fds 0-2): they are always open — an empty slot is the
 * implicit console. dup2 and F_DUPFD may use them as a SOURCE (the copy shares
 * the console description, so its F_GETFL equals the baseline F_GETFL of the
 * source: U3_BASEFL); only dup2 may target them, and the generator never does
 * (it would clobber an inherited fd). F_DUPFD never returns 0-2: arg 0..2 means
 * "lowest free fd >= 3". F_DUPFD arg < 0 -> -EINVAL; arg >= 32 -> -EMFILE.
 * F_GETFD always 0 and F_SETFD accepted (FD_CLOEXEC is not tracked: partial).
 *
 * Mode 5 programs model U3G_M ordinals and declare need_free = U3G_M (minimum):
 * they never hold more than U3G_LIVE_CAP fds, so every allocation the model
 * predicts lands inside the modelled ordinals regardless of extra free slots,
 * and they predict -EMFILE only from the absolute F_DUPFD bound. Mode 6 predicts
 * exhaustion, so it declares need_free = U3G_FD_BUDGET exactly.
 * =========================================================================== */
#define U3G_M          12
#define U3G_LIVE_CAP   8
#define U3G_F_DUPFD    0
#define U3G_F_GETFD    1
#define U3G_F_SETFD    2
#define U3G_F_GETFL    3
#define U3G_F_SETFL    4
#define U3G_O_WRONLY   0x1
#define U3G_O_RDWR     0x2
#define U3G_O_APPEND   0x400
#define U3G_O_NONBLOCK 0x800
#define U3G_O_CLOEXEC  0x80000
#define U3G_AT_FDCWD   (-100)
#define U3G_AF_INET    2
#define U3G_SOCK_DGRAM 2
#define U3G_EFAULT     14
#define U3G_EAFNOSUPPORT 97
#define U3G_PIPE_CAP   4096
#define U3G_SLOT_HI    (1L << 32)        /* high bits a 32-bit fd arg must drop */

enum { U3G_D_FILE, U3G_D_PR, U3G_D_PW, U3G_D_CON, U3G_D_SOCK };
typedef struct { int kind, flags, pipe, refs, con_src; } u3g_desc;
typedef struct { int buffered, readers, writers; } u3g_pipe_t;
static u3g_desc    u3g_dsc[U3_MAX_H + 2];
static u3g_pipe_t  u3g_pp[U3_MAX_H];
static int u3g_ndsc, u3g_npp, u3g_m;
static int u3g_odesc[32], u3g_oh[32];   /* ordinal -> desc id / bound handle, -1 free */

static void u3g_fd_reset(int m) {
    u3g_ndsc = u3g_npp = 0; u3g_m = m;
    for (int o = 0; o < 32; o++) { u3g_odesc[o] = -1; u3g_oh[o] = -1; }
}
static int u3g_fd_lowest(int from) {               /* lowest free ordinal >= from */
    for (int o = from < 0 ? 0 : from; o < u3g_m; o++) if (u3g_odesc[o] < 0) return o;
    return -1;
}
static int u3g_fd_live(void) {
    int n = 0;
    for (int o = 0; o < u3g_m; o++) if (u3g_odesc[o] >= 0) n++;
    return n;
}
static int u3g_fd_pick_live(u3g_rng* r) {          /* a random live ordinal, or -1 */
    int n = u3g_fd_live();
    if (!n) return -1;
    int k = (int)u3g_below(r, n);
    for (int o = 0; o < u3g_m; o++) if (u3g_odesc[o] >= 0 && k-- == 0) return o;
    return -1;
}
static int u3g_fd_pick_free(u3g_rng* r) {          /* a random free modelled ordinal */
    int n = u3g_m - u3g_fd_live();
    if (n <= 0) return -1;
    int k = (int)u3g_below(r, n);
    for (int o = 0; o < u3g_m; o++) if (u3g_odesc[o] < 0 && k-- == 0) return o;
    return -1;
}
static int u3g_desc_new(int kind, int flags, int pipe) {
    int d = u3g_ndsc < U3_MAX_H + 2 ? u3g_ndsc++ : (u3g_overflow = 1, U3_MAX_H + 1);
    u3g_dsc[d].kind = kind; u3g_dsc[d].flags = flags; u3g_dsc[d].pipe = pipe;
    u3g_dsc[d].refs = 0; u3g_dsc[d].con_src = 0;
    return d;
}
static void u3g_desc_ref(int d) { u3g_dsc[d].refs++; }
static void u3g_desc_unref(int d) {                 /* last ref: a pipe end detaches */
    if (--u3g_dsc[d].refs > 0) return;
    if (u3g_dsc[d].kind == U3G_D_PR) u3g_pp[u3g_dsc[d].pipe].readers--;
    if (u3g_dsc[d].kind == U3G_D_PW) u3g_pp[u3g_dsc[d].pipe].writers--;
}
static void u3g_fd_install(int o, int d, int h) { u3g_odesc[o] = d; u3g_oh[o] = h; u3g_desc_ref(d); }
static void u3g_fd_vacate(int o) {
    int d = u3g_odesc[o];
    u3g_odesc[o] = -1; u3g_oh[o] = -1;
    if (d >= 0) u3g_desc_unref(d);
}
/* Expected F_GETFL of a description. */
static void u3g_expect_fl(u3_call* c, int d) {
    if (u3g_dsc[d].kind == U3G_D_CON) { c->expect_mode = U3_BASEFL; c->expect_val = u3g_dsc[d].con_src; }
    else                              { c->expect_mode = U3_EXACT;  c->expect_val = u3g_dsc[d].flags; }
}
static void u3g_set_fd(u3_call* c, int i, int o) { u3g_set(c, i, U3_RES, u3g_oh[o]); }

/* --- creators: each binds a new handle at the model's lowest free ordinal --- */
static void u3g_fx_open(int at) {                  /* open / openat of /bin/hello */
    int o = u3g_fd_lowest(0), h = u3g_new_handle("fd");
    u3_call* c;
    if (at) {
        c = u3g_begin(U3G_OPENAT, 3, U3_BIND_FD, h);
        u3g_set(c, 0, U3_IMM, U3G_AT_FDCWD); u3g_set(c, 1, U3_PATH, 0);
        u3g_set(c, 2, U3_IMM, U3G_O_RDONLY);
    } else {
        c = u3g_begin(U3G_OPEN, 2, U3_BIND_FD, h);
        u3g_set(c, 0, U3_PATH, 0); u3g_set(c, 1, U3_IMM, U3G_O_RDONLY);
    }
    c->expect_mode = U3_SLOT; c->expect_val = o;
    u3g_fd_install(o, u3g_desc_new(U3G_D_FILE, U3G_O_RDONLY, 0), h);
}
static void u3g_fx_socket(void) {                  /* AF_INET datagram socket */
    int o = u3g_fd_lowest(0), h = u3g_new_handle("fd");
    u3_call* c = u3g_begin(U3G_SOCKET, 3, U3_BIND_FD, h);
    u3g_set(c, 0, U3_IMM, U3G_AF_INET); u3g_set(c, 1, U3_IMM, U3G_SOCK_DGRAM);
    u3g_set(c, 2, U3_IMM, 0);
    c->expect_mode = U3_SLOT; c->expect_val = o;
    u3g_fd_install(o, u3g_desc_new(U3G_D_SOCK, U3G_O_RDWR, 0), h);
}
static void u3g_fx_pipe(int two, int pbuf, int flags) {
    int o0 = u3g_fd_lowest(0), o1;
    int h0 = u3g_new_handle("fd"), h1 = u3g_new_handle("fd");
    u3_call* c = u3g_begin(two ? U3G_PIPE2 : U3G_PIPE, two ? 2 : 1, U3_BIND_PIPE, h0);
    c->bind_h1 = h1; c->bind_buf = pbuf;
    u3g_set(c, 0, U3_BUF, pbuf);
    if (two) u3g_set(c, 1, U3_IMM, flags);         /* flags are not honoured (partial) */
    c->expect_mode = U3_EXACT; c->expect_val = 0;
    int p = u3g_npp < U3_MAX_H ? u3g_npp++ : (u3g_overflow = 1, U3_MAX_H - 1);
    u3g_pp[p].buffered = 0; u3g_pp[p].readers = 1; u3g_pp[p].writers = 1;
    u3g_fd_install(o0, u3g_desc_new(U3G_D_PR, 0, p), h0);   /* read end installed first */
    o1 = u3g_fd_lowest(0);
    u3g_fd_install(o1, u3g_desc_new(U3G_D_PW, U3G_O_WRONLY, p), h1);
}
/* dup2(live src, free slot k): returns slot k, sharing src's description. */
static void u3g_fx_dup2_free(u3g_rng* r, u3g_rng* m) {
    int so = u3g_fd_pick_live(r), ko = u3g_fd_pick_free(r);
    int h = u3g_new_handle("fd");
    u3_call* c = u3g_begin(U3G_DUP2, 2, U3_BIND_FD, h);
    u3g_set_fd(c, 0, so);
    long enc = ko | ((u3g_below(m, 4) == 0) ? U3G_SLOT_HI : 0);  /* int truncation */
    u3g_set(c, 1, U3_FDSLOT, enc);
    c->expect_mode = U3_SLOT; c->expect_val = ko;
    u3g_fd_install(ko, u3g_odesc[so], h);
}
/* dup2(live src, live dst): dst's old description is released, dst now shares
 * src's; dst keeps its handle. dst == src returns it unchanged. */
static void u3g_fx_dup2_live(u3g_rng* r) {
    int so = u3g_fd_pick_live(r), dob = u3g_fd_pick_live(r);
    u3_call* c = u3g_begin(U3G_DUP2, 2, U3_BIND_NONE, 0);
    u3g_set_fd(c, 0, so); u3g_set_fd(c, 1, dob);
    c->expect_mode = U3_SLOT; c->expect_val = dob;
    if (so != dob) {
        int d = u3g_odesc[so], h = u3g_oh[dob];
        u3g_desc_ref(d);                               /* ref before unref: same-desc safe */
        u3g_desc_unref(u3g_odesc[dob]);
        u3g_odesc[dob] = d; u3g_oh[dob] = h;
    }
}
/* dup2 / F_DUPFD from a console fd (1 or 2): a copy of the console. */
static void u3g_fx_dup_console(u3g_rng* r, u3g_rng* m) {
    int src = 1 + (int)u3g_below(m, 2), ko, h = u3g_new_handle("fd");
    u3_call* c;
    if (u3g_below(m, 2)) {
        ko = u3g_fd_pick_free(r);
        c = u3g_begin(U3G_DUP2, 2, U3_BIND_FD, h);
        u3g_set(c, 0, U3_IMM, src); u3g_set(c, 1, U3_FDSLOT, ko);
    } else {
        ko = u3g_fd_lowest(0);
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_FD, h);
        u3g_set(c, 0, U3_IMM, src); u3g_set(c, 1, U3_IMM, U3G_F_DUPFD);
        u3g_set(c, 2, U3_IMM, 3);
    }
    c->expect_mode = U3_SLOT; c->expect_val = ko;
    int d = u3g_desc_new(U3G_D_CON, 0, 0);
    u3g_dsc[d].con_src = src;
    u3g_fd_install(ko, d, h);
}
/* F_DUPFD(live, floor): floor is a baseline slot, or 0..2 (-> lowest >= 3). */
static void u3g_fx_dupfd(u3g_rng* r, u3g_rng* m) {
    int so = u3g_fd_pick_live(r), fl, ko, h;
    int lowfloor = (int)u3g_below(m, 2);
    if (lowfloor) { fl = (int)u3g_below(m, 3); ko = u3g_fd_lowest(0); }
    else {
        fl = (int)u3g_below(r, u3g_m);
        ko = u3g_fd_lowest(fl);
        if (ko < 0) { fl = 0; ko = u3g_fd_lowest(0); }
    }
    h = u3g_new_handle("fd");
    u3_call* c = u3g_begin(U3G_FCNTL, 3, U3_BIND_FD, h);
    u3g_set_fd(c, 0, so); u3g_set(c, 1, U3_IMM, U3G_F_DUPFD);
    if (lowfloor) u3g_set(c, 2, U3_IMM, fl); else u3g_set(c, 2, U3_FDSLOT, fl);
    c->expect_mode = U3_SLOT; c->expect_val = ko;
    u3g_fd_install(ko, u3g_odesc[so], h);
}
/* --- non-allocating fcntl --- */
static void u3g_fx_fcntl_flags(u3g_rng* r, u3g_rng* m) {
    int o = u3g_fd_pick_live(r), d = u3g_odesc[o];
    int what = (int)u3g_below(m, 4);
    if (what == 1 && u3g_dsc[d].kind == U3G_D_CON) what = 0;  /* never F_SETFL a shared console */
    u3_call* c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
    u3g_set_fd(c, 0, o);
    if (what == 0) {                                   /* F_GETFL */
        u3g_set(c, 1, U3_IMM, U3G_F_GETFL);
        u3g_expect_fl(c, d);
    } else if (what == 1) {                            /* F_SETFL: only APPEND|NONBLOCK change */
        static const int args[4] = { 0, U3G_O_APPEND, U3G_O_NONBLOCK,
                                     U3G_O_APPEND | U3G_O_NONBLOCK | U3G_O_RDWR | U3G_O_WRONLY };
        int a = args[u3g_below(m, 4)];
        u3g_set(c, 1, U3_IMM, U3G_F_SETFL); u3g_set(c, 2, U3_IMM, a);
        c->expect_mode = U3_EXACT; c->expect_val = 0;
        int keep = U3G_O_APPEND | U3G_O_NONBLOCK;
        u3g_dsc[d].flags = (u3g_dsc[d].flags & ~keep) | (a & keep);
    } else if (what == 2) {                            /* F_GETFD: CLOEXEC not tracked */
        u3g_set(c, 1, U3_IMM, U3G_F_GETFD);
        c->expect_mode = U3_EXACT; c->expect_val = 0;
    } else {                                           /* F_SETFD(FD_CLOEXEC): accepted */
        u3g_set(c, 1, U3_IMM, U3G_F_SETFD); u3g_set(c, 2, U3_IMM, 1);
        c->expect_mode = U3_EXACT; c->expect_val = 0;
    }
}
/* --- pipe data: only calls that cannot block or raise SIGPIPE --- */
static int u3g_fx_pipe_io(u3g_rng* r, int wbuf, int rbuf) {
    int cand[32], n = 0;
    for (int o = 0; o < u3g_m; o++) {
        int d = u3g_odesc[o];
        if (d < 0) continue;
        u3g_pipe_t* p = &u3g_pp[u3g_dsc[d].pipe];
        if (u3g_dsc[d].kind == U3G_D_PW && p->readers > 0 && p->buffered + 64 <= U3G_PIPE_CAP) cand[n++] = o;
        if (u3g_dsc[d].kind == U3G_D_PR && (p->buffered > 0 || p->writers == 0)) cand[n++] = o;
    }
    if (!n) return 0;
    int o = cand[u3g_below(r, n)], d = u3g_odesc[o];
    u3g_pipe_t* p = &u3g_pp[u3g_dsc[d].pipe];
    int len = 1 + (int)u3g_below(r, 64);
    u3_call* c;
    if (u3g_dsc[d].kind == U3G_D_PW) {
        c = u3g_begin(U3G_WRITE, 3, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, o); u3g_set(c, 1, U3_BUF, wbuf); u3g_set(c, 2, U3_IMM, len);
        c->expect_mode = U3_EXACT; c->expect_val = len;
        p->buffered += len;
    } else {
        c = u3g_begin(U3G_READ, 3, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, o); u3g_set(c, 1, U3_BUF, rbuf); u3g_set(c, 2, U3_IMM, len);
        int got = len < p->buffered ? len : p->buffered;
        c->expect_mode = U3_EXACT; c->expect_val = got;
        p->buffered -= got;
    }
    return 1;
}
static void u3g_fx_close(int o) {
    u3_call* c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
    u3g_set_fd(c, 0, o);
    c->expect_mode = U3_EXACT; c->expect_val = 0;
    u3g_fd_vacate(o);
}
/* --- adversarial calls: refused, so the model does not change --- */
static void u3g_fx_adversarial(u3g_rng* r, u3g_rng* m, int pbuf) {
    static const long badptr[4] = { 0, (long)0xffffffff80000000UL,
                                    (long)0x0000800000000000UL, 0x1000 };
    int live = u3g_fd_pick_live(r), fr = u3g_fd_pick_free(r);
    int pick = (int)u3g_below(m, 12);
    if (live < 0 && (pick >= 3 && pick <= 6)) pick = 0;   /* needs a live fd */
    if (fr < 0 && (pick == 2 || pick == 8)) pick = 0;     /* needs a free slot */
    u3_call* c;
    (void)pbuf;
    switch (pick) {
    case 0: case 1: {                                     /* pipe/pipe2(bad ptr) */
        int two = pick;
        c = u3g_begin(two ? U3G_PIPE2 : U3G_PIPE, two ? 2 : 1, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_BADPTR, badptr[u3g_below(m, 4)]);
        if (two) u3g_set(c, 1, U3_IMM, 0);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EFAULT;
        break;
    }
    case 2:                                               /* dup2(free/stale, free) */
        c = u3g_begin(U3G_DUP2, 2, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_FDSLOT, fr); u3g_set(c, 1, U3_FDSLOT, fr);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    case 3: {                                             /* dup2(live, out-of-range) */
        static const long nf[3] = { -1, 32, 0x7fffffff };
        c = u3g_begin(U3G_DUP2, 2, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, live); u3g_set(c, 1, U3_IMM, nf[u3g_below(m, 3)]);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    }
    case 4:                                               /* invalid fcntl cmd */
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, live); u3g_set(c, 1, U3_IMM, 999);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EINVAL;
        break;
    case 5:                                               /* F_DUPFD floor < 0 */
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, live); u3g_set(c, 1, U3_IMM, U3G_F_DUPFD); u3g_set(c, 2, U3_IMM, -1);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EINVAL;
        break;
    case 6: {                                             /* F_DUPFD floor >= 32 */
        static const long fl[3] = { 32, 0x80000000L, 0x7fffffff };
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
        u3g_set_fd(c, 0, live); u3g_set(c, 1, U3_IMM, U3G_F_DUPFD);
        u3g_set(c, 2, U3_IMM, fl[u3g_below(m, 3)]);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EMFILE;
        break;
    }
    case 7: {                                             /* fcntl on a bad fd */
        static const long bf[3] = { -1, 32, 0x7fffffff };
        c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_IMM, bf[u3g_below(m, 3)]); u3g_set(c, 1, U3_IMM, U3G_F_GETFL);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    }
    case 8:                                               /* close/fcntl of a free slot */
        if (u3g_below(m, 2)) {
            c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
            u3g_set(c, 0, U3_FDSLOT, fr);
        } else {
            c = u3g_begin(U3G_FCNTL, 3, U3_BIND_NONE, 0);
            u3g_set(c, 0, U3_FDSLOT, fr); u3g_set(c, 1, U3_IMM, U3G_F_GETFL);
        }
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    case 9:                                               /* dup2(-1, free) */
        c = u3g_begin(U3G_DUP2, 2, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_IMM, -1); u3g_set(c, 1, U3_IMM, 31);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    case 10:                                              /* unsupported family */
        c = u3g_begin(U3G_SOCKET, 3, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_IMM, 99); u3g_set(c, 1, U3_IMM, U3G_SOCK_DGRAM); u3g_set(c, 2, U3_IMM, 0);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EAFNOSUPPORT;
        break;
    default:                                              /* close(-1) */
        c = u3g_begin(U3G_CLOSE, 1, U3_BIND_NONE, 0);
        u3g_set(c, 0, U3_IMM, -1);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EBADF;
        break;
    }
}

static void u3g_fd_prog_finish(const char* name, int need_free, int need_exact) {
    u3g_prog.name = name; u3g_prog.expect_fault = 0; u3g_prog.replay = 1;
    u3g_prog.need_free = need_free; u3g_prog.need_exact = need_exact;
    u3g_prog.nhandles = u3g_nhandles;
    u3g_prog.bufs = u3g_bufs;   u3g_prog.nbufs = u3g_nbufs;
    u3g_prog.paths = u3g_paths; u3g_prog.npaths = 1;
    u3g_prog.iovs = 0;          u3g_prog.niovs = 0;
    u3g_prog.calls = u3g_calls;
    u3g_prog.ncalls = u3g_overflow ? (U3_MAX_CALLS + 1) : u3g_ncalls;
    u3g_prog.dcs = 0;           u3g_prog.ndcs = 0;
}

#define U3G_FD_STEPS 18
/* Mode 5 "fdops": base_seed fixes which operation each step performs and on
 * which fds; mut_seed picks argument VALUES (flags, floors, bad pointers,
 * adversarial variants, the int-truncated dup2 target). */
static const u3_program* u3g_build_fdops(unsigned long base_seed, unsigned long mut_seed) {
    u3g_rng r = { base_seed }, m = { mut_seed };
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;
    u3g_overflow = 0;
    u3g_fd_reset(U3G_M);
    int pbuf = u3g_new_buf(&r, 8, 0);          /* pipefd[2], reused by every pipe call */
    int wbuf = u3g_new_buf(&r, 64, 1);         /* write source */
    int rbuf = u3g_new_buf(&r, 64, 0);         /* read destination */

    for (int step = 0; step < U3G_FD_STEPS; step++) {
        int live = u3g_fd_live();
        int room = (live + 2 <= U3G_LIVE_CAP) && (u3g_nhandles + 2 <= U3_MAX_H);
        int op = (int)u3g_below(&r, 11);
        if (u3g_ncalls + live + 2 >= U3G_MAX_CALLS) break;
        if (!live && op >= 4 && op <= 8) op = 0;           /* those need a live fd */
        if (!room && (op <= 5 || op == 9)) op = 10;        /* those allocate */
        switch (op) {
        case 0:  u3g_fx_open((int)u3g_below(&m, 2)); break;
        case 1:  u3g_fx_pipe((int)u3g_below(&m, 2), pbuf,
                             u3g_below(&m, 2) ? U3G_O_CLOEXEC : 0); break;
        case 2:  if (room && live) { u3g_fx_dup2_free(&r, &m); break; }
                 u3g_fx_socket(); break;
        case 3:  u3g_fx_socket(); break;
        case 4:  u3g_fx_dupfd(&r, &m); break;
        case 5:  u3g_fx_dup2_free(&r, &m); break;
        case 6:  u3g_fx_dup2_live(&r); break;
        case 7:  u3g_fx_fcntl_flags(&r, &m); break;
        case 8:  if (!u3g_fx_pipe_io(&r, wbuf, rbuf)) u3g_fx_fcntl_flags(&r, &m); break;
        case 9:  u3g_fx_dup_console(&r, &m); break;
        default: if (live && u3g_below(&r, 2)) u3g_fx_close(u3g_fd_pick_live(&r));
                 else u3g_fx_adversarial(&r, &m, pbuf);
                 break;
        }
    }
    /* Drain pipes whose writers are gone (a read can never block then) and close
     * everything: the fd table must end exactly at baseline. */
    for (int o = u3g_m - 1; o >= 0; o--) if (u3g_odesc[o] >= 0) u3g_fx_close(o);
    u3g_fd_prog_finish("gen_fdops", U3G_M, 0);
    return &u3g_prog;
}

/* Mode 6 "pipe-rollback": deterministic exercise of every vfs_pipe failure path
 * reachable from Ring 3, each followed by an allocation whose EXACT slot proves
 * the failed pipe left nothing behind:
 *   pipe(NULL) -> -EFAULT (do_pipe closes both ends it had installed);
 *   with exactly ONE slot free: pipe -> -EMFILE after the read end was installed
 *     (the write-end install fails; the read end must be rolled back), then an
 *     open must get that last slot;
 *   with NO slot free: pipe -> -EMFILE at the first install.
 * It predicts exhaustion, so it requires exactly U3G_FD_BUDGET free slots. */
static const u3_program* u3g_build_piperb(void) {
    u3g_rng r = { 1 };
    u3g_ncalls = u3g_nbufs = u3g_nhandles = u3g_nfd = u3g_naddr = 0;
    u3g_overflow = 0;
    u3g_fd_reset(U3G_FD_BUDGET);
    int pbuf = u3g_new_buf(&r, 8, 0);
    u3_call* c = u3g_begin(U3G_PIPE, 1, U3_BIND_NONE, 0);
    u3g_set(c, 0, U3_BADPTR, 0);
    c->expect_mode = U3_EXACT; c->expect_val = -U3G_EFAULT;
    for (int i = 0; i < U3G_FD_BUDGET - 1; i++) u3g_fx_open(0);   /* slots 0..27 */
    for (int pass = 0; pass < 2; pass++) {
        int h0 = u3g_new_handle("fd"), h1 = u3g_new_handle("fd");
        c = u3g_begin(U3G_PIPE, 1, U3_BIND_PIPE, h0);
        c->bind_h1 = h1; c->bind_buf = pbuf;
        u3g_set(c, 0, U3_BUF, pbuf);
        c->expect_mode = U3_EXACT; c->expect_val = -U3G_EMFILE;
        if (pass == 0) u3g_fx_open(0);                             /* must be slot 28 */
    }
    for (int o = u3g_m - 1; o >= 0; o--) if (u3g_odesc[o] >= 0) u3g_fx_close(o);
    u3g_fd_prog_finish("gen_piperb", U3G_FD_BUDGET, 1);
    return &u3g_prog;
}

#endif /* MAKHOS_UFUZZ_U3_GEN_H */
