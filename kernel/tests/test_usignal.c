/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_usignal.c
 * Phase 20-G: user-installed signal handlers. Each program encodes the whole
 * outcome in its exit status, so the kernel just reads it back.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <signal.h>

/* /bin/sigtest installs a SIGTERM handler, raises it, and exits 42 iff the
 * handler ran and sigreturn resumed the mainline. */
KTEST(usignal, handler_runs_and_sigreturn_resumes) {
    int pid = proc_spawn_user("/bin/sigtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}

/* /bin/sigmask proves a blocked signal stays pending until unblocked: the
 * mainline digit (1) lands before the handler digit (2), so the status is 12. */
KTEST(usignal, sigprocmask_defers_delivery) {
    int pid = proc_spawn_user("/bin/sigmask");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 12);
}

/* AUD-02/03b: sigreturn restores RIP/RSP from the user-controlled frame and the
 * kernel returns via SYSRET, which #GPs in ring 0 on a non-canonical RIP. The
 * validator must reject a non-canonical or kernel-half return state. */
KTEST(usignal, restore_rejects_noncanonical_state) {
    KEXPECT_EQ(signal_restore_ok(0x400000ULL, 0x7ffffff000ULL), 1);        /* valid user */
    KEXPECT_EQ(signal_restore_ok(0xFFFF800000000000ULL, 0x400000ULL), 0);  /* kernel RIP */
    KEXPECT_EQ(signal_restore_ok(0x0000800000000000ULL, 0x400000ULL), 0);  /* non-canonical RIP */
    KEXPECT_EQ(signal_restore_ok(0x400000ULL, 0xFFFFFFFFFFFFF000ULL), 0);   /* bad RSP */
    KEXPECT_EQ(signal_restore_ok(0ULL, 0x400000ULL), 0);                    /* null RIP */
}

/* AUD-03a: a blocked SIGKILL must still terminate — a crafted sigreturn could
 * set the SIGKILL bit in the mask, but delivery honors uncatchable signals. */
KTEST(usignal, sigkill_delivers_even_if_mask_blocks_it) {
    process_t* cur = current_process;
    KASSERT_TEST(cur != NULL);
    uint64_t sb = cur->sig_blocked, sp = cur->sig_pending, h = cur->sig_handlers[SIGKILL];
    cur->sig_handlers[SIGKILL] = 0;
    cur->sig_blocked = sigmask(SIGKILL);          /* the crafted-sigreturn scenario */
    cur->sig_pending = sigmask(SIGKILL);
    int sig = signal_take_terminate();
    cur->sig_blocked = sb; cur->sig_pending = sp; cur->sig_handlers[SIGKILL] = h;
    KEXPECT_EQ(sig, SIGKILL);                      /* blocked SIGKILL still kills */
}

/* AUD-02/03, end to end: the checks above call the kernel helpers directly from
 * ring 0. /bin/sigfuzz drives the *real* rt_sigreturn path from ring 3 — it
 * installs a handler, raises SIGTERM, and from inside the handler rewrites the
 * saved frame the kernel is about to trust, then lets sigreturn run. Each mode
 * encodes its verdict in the exit status (see user/sigfuzz.c). This is what the
 * fuzzer/attacker actually reaches, so a regression in any one defense shows up
 * as a wrong status (or a kernel #GP), not just a failing unit assertion. */
static int run_sigfuzz(const char* mode) {
    char* argv[] = { "/bin/sigfuzz", (char*)mode, 0 };
    int pid = proc_spawn_user_argv("/bin/sigfuzz", argv);
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    return status;
}

/* AUD-03b: a forged non-canonical RIP must terminate the process cleanly with
 * the kernel intact (128 + SIGSEGV = 139). This asserts the end-to-end safe
 * outcome; it does not by itself prove signal_restore_ok fired, because a frame
 * that slipped past it would fault on the ring-3 fetch to the same 139 — the
 * ring-0 SYSRET #GP restore_ok really guards (CVE-2012-0217) is Intel-silicon
 * behavior QEMU-TCG does not model. The validator's logic is pinned by
 * restore_rejects_noncanonical_state above. */
KTEST(usignal, sigreturn_rejects_forged_rip_from_ring3) {
    KEXPECT_EQ(run_sigfuzz("rip"), 128 + SIGSEGV);
}

/* AUD-03b: same end-to-end property for a forged non-canonical RSP. */
KTEST(usignal, sigreturn_rejects_forged_rsp_from_ring3) {
    KEXPECT_EQ(run_sigfuzz("rsp"), 128 + SIGSEGV);
}

/* AUD-03a: a frame that blocks SIGKILL/SIGSTOP must come back with those bits
 * stripped; the child reads its own mask and exits 0 iff they are gone. */
KTEST(usignal, sigreturn_strips_uncatchable_from_forged_mask) {
    KEXPECT_EQ(run_sigfuzz("mask"), 0);
}

/* AUD-02: a frame with TF/IOPL/NT/AC/ID set must be sanitized; the child
 * resumes and exits 0 (TF left on would single-step it to death). */
KTEST(usignal, sigreturn_sanitizes_forged_rflags) {
    KEXPECT_EQ(run_sigfuzz("rflags"), 0);
}
