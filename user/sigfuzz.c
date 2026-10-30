/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * /bin/sigfuzz — a ring-3 adversary against the rt_sigreturn path (AUD-02/03).
 *
 * sigreturn rebuilds the interrupted register state from a frame that lives on
 * the *user* stack, i.e. a frame an attacker fully controls, and the kernel
 * returns to ring 3 through SYSRET — which #GPs in ring 0 (after swapgs) on a
 * non-canonical RIP (the CVE-2012-0217 class). The kernel defends in
 * signal_sigreturn():
 *   - signal_restore_ok() rejects a non-canonical / kernel-half RIP or RSP   (AUD-03b)
 *   - the restored RFLAGS is masked down to the user-settable bits + forced IF (AUD-02)
 *   - SIGKILL/SIGSTOP are stripped from the restored block mask              (AUD-03a)
 *
 * Those defenses were previously exercised only by white-box KTESTs that call
 * the kernel helpers directly from ring 0. This program drives the *real* path:
 * it installs a handler, raises a signal, and — from inside the handler, before
 * sigreturn runs — rewrites the saved context the kernel is about to trust.
 * The whole outcome is encoded in the exit status so the spawning KTEST just
 * reads it back (as /bin/sigtest and /bin/sigmask do):
 *
 *   mode     tamper                      fixed kernel       defense reverted
 *   ----     ------                      ------------       ----------------
 *   mask     block |= KILL|STOP          exit 0 (stripped)  exit 1 (still blocked)
 *   rflags   RFLAGS |= TF|NT|IOPL|AC|ID  exit 0 (sanitized) killed (TF single-steps)
 *   rip      RIP := non-canonical        killed -> 139      killed -> 139 (see below)
 *   rsp      RSP := non-canonical        killed -> 139      killed -> 139 (see below)
 *
 * 139 == 128 + SIGSEGV: thread_exit(128 + SIGSEGV).
 *
 * `mask` and `rflags` are true oracles: removing the kernel's strip/sanitize
 * flips the exit status, so the test catches the regression. `rip`/`rsp` assert
 * the end-to-end security property — a hostile non-canonical frame terminates
 * the process and the kernel survives — but they cannot, by exit status alone,
 * distinguish "rejected by signal_restore_ok" from "returned into and faulted in
 * ring 3": both end in a clean 139. The ring-0 SYSRET #GP that restore_ok guards
 * against (CVE-2012-0217) is an Intel-silicon behavior QEMU-TCG does not model,
 * so it is the white-box test (restore_rejects_noncanonical_state) that pins the
 * validator's logic; these two lock the safe end-to-end outcome.
 */
#include "usys.h"

#ifndef SIGSTOP
#define SIGSTOP 19          /* uncatchable stop (kernel signal.h); usys.h omits it */
#endif

/* The frame signal_deliver() writes and signal_sigreturn() reads, byte-for-byte
 * the kernel's sigcontext_t (kernel/signal/signal.c). The handler is entered
 * with %rsp pointing at the restorer slot; the frame sits 8 bytes above it. */
struct sigctx {
    unsigned long r15, r14, r13, r12, r11, r10, r9, r8;
    unsigned long rbp, rdi, rsi, rdx, rcx, rbx, rax;
    unsigned long rip, rflags, rsp;
    unsigned long blocked;
};

enum { M_RIP = 0, M_RSP, M_MASK, M_RFLAGS };
static volatile int g_mode;

/* RFLAGS bits a hostile frame would love the kernel to honor. */
#define RF_TF    (1UL << 8)    /* trap flag: single-step on resume            */
#define RF_IOPL3 (3UL << 12)   /* I/O privilege level 3: cli/sti/in/out in r3 */
#define RF_NT    (1UL << 14)   /* nested task                                 */
#define RF_AC    (1UL << 18)   /* alignment check                             */
#define RF_ID    (1UL << 21)   /* cpuid-serializing flag                      */

/* Called by the asm shim below with a pointer to the kernel's about-to-be-
 * trusted frame. We only rewrite the field the current mode targets; the rest
 * stays as the kernel saved it, so the "resume" modes return to live C code. */
void sigfuzz_tamper(int signo, struct sigctx* sc) {
    (void)signo;
    switch (g_mode) {
    case M_RIP:    sc->rip = 0x0000800000000000UL;                 break; /* non-canonical */
    case M_RSP:    sc->rsp = 0x0000800000000000UL;                 break; /* non-canonical */
    case M_MASK:   sc->blocked |= sigmask(SIGKILL) | sigmask(SIGSTOP); break;
    case M_RFLAGS: sc->rflags |= RF_TF | RF_IOPL3 | RF_NT | RF_AC | RF_ID; break;
    }
}

/*
 * The signal handler, in assembly so the frame geometry is exact. On entry
 * %rsp = the restorer slot and the saved frame is at 8(%rsp). We hand that
 * pointer to sigfuzz_tamper() (16-aligning the stack across the call) and then
 * `ret` into the restorer the kernel installed — the normal sigreturn path.
 */
__asm__(
    ".text\n"
    ".globl sigfuzz_handler\n"
    "sigfuzz_handler:\n"
    "    lea 8(%rsp), %rsi\n"   /* arg2 = &sigctx (above the restorer slot)   */
    "    sub $8, %rsp\n"        /* 16-align for the call (entry %rsp%16 == 8) */
    "    call sigfuzz_tamper\n" /* arg1 = signo, already in %rdi              */
    "    add $8, %rsp\n"        /* back to the restorer slot                  */
    "    ret\n"                 /* -> __sigreturn_trampoline -> rt_sigreturn  */
);
extern void sigfuzz_handler(int);

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int umain(int argc, char** argv) {
    int mode;
    if (argc < 2)                         mode = M_RIP;
    else if (streq(argv[1], "rip"))       mode = M_RIP;
    else if (streq(argv[1], "rsp"))       mode = M_RSP;
    else if (streq(argv[1], "mask"))      mode = M_MASK;
    else if (streq(argv[1], "rflags"))    mode = M_RFLAGS;
    else                                  return 2;    /* unknown mode */

    g_mode = mode;
    usignal(SIGTERM, (void (*)(int))sigfuzz_handler);
    uraise(SIGTERM);    /* delivered at return -> sigfuzz_handler -> tamper -> sigreturn */

    /* Only the non-fatal modes reach here; rip/rsp are killed by the validator
     * (and if a reverted kernel let them through, it #GPs in ring 0 — we are
     * long gone either way, so reaching this with those modes is itself wrong). */
    switch (mode) {
    case M_MASK: {
        /* sigreturn must have stripped SIGKILL/SIGSTOP from the mask it restored
         * from our forged frame; read the live mask back and insist on it. */
        unsigned long cur = ~0UL;
        if (usigprocmask(SIG_BLOCK, 0, &cur) != 0) return 3;
        return (cur & (sigmask(SIGKILL) | sigmask(SIGSTOP))) ? 1 : 0;
    }
    case M_RFLAGS:
        /* We resumed at all: TF was stripped (else we'd have trapped on the
         * first instruction after SYSRET) and IOPL/NT/AC/ID did no harm. */
        return 0;
    default:
        return 7;   /* rip/rsp should never return from sigreturn */
    }
}
