/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_tls.c
 * Phase 20-H: arch_prctl(ARCH_SET_FS). /bin/tlstest points FS at a block, reads
 * it back through %fs (a real TLS access) and via ARCH_GET_FS, exiting 55 iff
 * the thread pointer survives the syscall boundary and any preemption.
 */

#include <ktest.h>
#include <proc_internal.h>

KTEST(tls, arch_prctl_set_fs_round_trip) {
    int pid = proc_spawn_user("/bin/tlstest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 55);
}

/* T-S01: arch_prctl(ARCH_SET_FS) must reject a non-canonical / kernel-half base
 * instead of wrmsr'ing it in ring 0. /bin/fsguard tries several bad bases (each
 * must return < 0) then a legal one (must round-trip), exiting 55 on success.
 *
 * Fail-first: remove the `user_fs_base_ok` guard in do_arch_prctl and the first
 * bad SET_FS #GPs on wrmsr in ring 0 -> exception_report (noreturn) -> the
 * kernel panics -> /bin/fsguard never exits -> sys_waitpid never returns ->
 * the suite times out. (This is a ring-3-triggerable kernel DoS without the
 * guard; verified by hand.) Not timing-sensitive, so it runs under stress too. */
KTEST(tls, set_fs_rejects_noncanonical) {
    int pid = proc_spawn_user("/bin/fsguard");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 55);
}
