/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_fork.c
 * Phase 20-A-2: fork() and execve() driven end to end by real ring-3 programs.
 * Each helper forks (and some then execve), waits, and returns 0 only if every
 * invariant held; a nonzero status names the step that broke.
 *
 * Registered in reverse of run order, so the simplest case (a bare fork) runs
 * first and the many-iteration leak check runs last.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <mm/pmm.h>

/* Churn fork+exec+wait many times: every address space, stack and pid for both
 * the forked child and the exec'd image must be reclaimed. */
KTEST(fork, fork_exec_many_no_leak) {
    size_t base = pmm_get_free_memory();
    for (int i = 0; i < 20; i++) {
        int pid = proc_spawn_user("/bin/forkexec");
        KASSERT_TEST(pid > 0);
        int status = -1;
        sys_waitpid(pid, &status);
        KEXPECT_EQ(status, 0);
    }
    KEXPECT_EQ(pmm_get_free_memory(), base);
}

/* fork()+execve(): the child replaces itself with /bin/hello (exit 42). */
KTEST(fork, fork_then_execve_runs_new_image) {
    int pid = proc_spawn_user("/bin/forkexec");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);
}

/* fork() COW: the child mutates a .data global; the parent's copy is untouched. */
KTEST(fork, cow_isolates_child_writes) {
    int pid = proc_spawn_user("/bin/forkcow");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);
}

/* fork(): child exits 7, parent waitpid's it and sees the code. */
KTEST(fork, parent_waits_for_child) {
    int pid = proc_spawn_user("/bin/forktest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);
}
