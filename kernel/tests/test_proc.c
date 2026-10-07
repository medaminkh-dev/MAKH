/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_proc.c
 * Phase 20-A: real user processes — load an ELF from the initrd, run it in
 * ring 3 preemptively, and collect its exit status with waitpid.
 *
 * Tests are registered in reverse of execution order, so the simplest cases
 * (single spawn + wait) run first and the harder ones (two live address
 * spaces, preemption) run last.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <mm/pmm.h>
#include <drivers/timer.h>

KTEST(proc, user_fault_becomes_sigsegv) {
    int pid = proc_spawn_user("/bin/faulter");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 128 + SIGSEGV);      /* killed by SIGSEGV -> 139 */
}

KTEST(proc, ring3_is_preemptible) {
    uint64_t t0 = timer_get_ticks();
    int pid = proc_spawn_user("/bin/spin");  /* CPU-bound loop in ring 3 */
    KASSERT_TEST(pid > 0);
    /* We must wake from this sleep even though a user process is spinning:
     * the timer has to be preempting ring 3. If IF were masked in user mode,
     * the timer would never fire and this sleep would hang the whole run. */
    sched_sleep_ms(30);
    KEXPECT(timer_get_ticks() > t0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 9);
}

KTEST(proc, two_processes_distinct_address_spaces) {
    int a = proc_spawn_user("/bin/hello");
    int b = proc_spawn_user("/bin/getpid");
    KASSERT_TEST(a > 0 && b > 0 && a != b);
    int s1 = -1, s2 = -1;
    int w1 = sys_waitpid(-1, &s1);          /* reap whichever finishes */
    int w2 = sys_waitpid(-1, &s2);
    KEXPECT(w1 > 0 && w2 > 0 && w1 != w2);
    KEXPECT_EQ(sys_waitpid(-1, 0), -ECHILD);/* no children left */
}

KTEST(proc, exit_code_from_getpid) {
    int pid = proc_spawn_user("/bin/getpid");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, pid & 0x7f);         /* program exits with its own pid */
}

KTEST(proc, spawn_elf_and_wait) {
    int pid = proc_spawn_user("/bin/hello");
    KASSERT_TEST(pid > 0);
    int status = -1;
    KEXPECT_EQ(sys_waitpid(pid, &status), pid);
    KEXPECT_EQ(status, 42);                 /* hello returns 42 */
}

/* Churn the whole lifecycle many times: every address space, kernel stack,
 * pid and page table must come back, or free memory would drift down. */
KTEST(proc, spawn_wait_many_no_leak) {
    size_t base = pmm_get_free_memory();
    for (int i = 0; i < 40; i++) {
        int pid = proc_spawn_user("/bin/getpid");
        KASSERT_TEST(pid > 0);
        int status = -1;
        KEXPECT_EQ(sys_waitpid(pid, &status), pid);
        KEXPECT_EQ(status, pid & 0x7f);
    }
    KEXPECT_EQ(pmm_get_free_memory(), base);   /* net-zero: no process leaks */
}

/* A path that does not exist is a clean -ENOENT, not a crash. */
KTEST(proc, spawn_missing_path_is_enoent) {
    KEXPECT_EQ(proc_spawn_user("/bin/does-not-exist"), -ENOENT);
}
