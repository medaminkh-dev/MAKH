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
