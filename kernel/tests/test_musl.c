/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_musl.c
 * Phase 20-O (F20): the headline milestone — a real C program, built against
 * musl libc as a static-PIE and run on MAKH unchanged, reaches main and exits
 * cleanly. /bin/muslhello exits 42 iff crt startup + self-relocation, TLS via
 * %fs (a __thread variable), the heap (malloc), buffered stdio (printf through
 * writev) and the exit path all worked end to end. Any libc-ABI gap shows up
 * as a wrong exit code (its internal 1/2/3 checks) or a fault, never a pass.
 */

#include <ktest.h>
#include <proc_internal.h>

KTEST(musl, static_pie_hello_runs) {
    int pid = proc_spawn_user("/bin/muslhello");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}
