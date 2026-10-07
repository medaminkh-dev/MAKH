/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_thread.c
 * Phase 20-L: clone + futex + join. /bin/threadtest spawns a worker thread that
 * shares memory and fds with main; both bump a shared counter 20000 times under
 * a futex mutex, then main joins via the CLEARTID futex. A correct counter of
 * 40000 (no lost updates) and a clean join give exit 55.
 */

#include <ktest.h>
#include <proc_internal.h>

KTEST(thread, clone_futex_mutex_and_join) {
    int pid = proc_spawn_user("/bin/threadtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 55);
}
