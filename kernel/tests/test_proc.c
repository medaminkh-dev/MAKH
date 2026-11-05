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

/* F21 Path A: a standard non-PIE ET_EXEC linked at the conventional low address
 * 0x400000 (PML4 slot 0) loads and runs. Before the higher-half migration slot 0
 * was the shared kernel identity map, so a low binary could not load; now the
 * whole lower canonical half is the process's own. */
KTEST(proc, standard_low_binary_runs) {
    int pid = proc_spawn_user("/bin/lowexec");
    KASSERT_TEST(pid > 0);
    int status = -1;
    KEXPECT_EQ(sys_waitpid(pid, &status), pid);
    KEXPECT_EQ(status, 77);                 /* lowexec returns 77 from 0x400000 */
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

/* -------- T-P01: waitpid is interruptible by a signal (EINTR) -------- */

static volatile int tp01_rc;
static volatile int tp01_child_pid;
static volatile int tp01_ready;

/* Parent worker: spawn a never-exiting child, then block in waitpid(). A signal
 * from the main thread must make that waitpid return -EINTR. We then reap our
 * own child so nothing leaks into later tests. */
static void tp01_parent(void* arg) {
    (void)arg;
    int cpid = proc_spawn_user("/bin/spinner");     /* loops forever until killed */
    tp01_child_pid = cpid;
    if (cpid <= 0) { tp01_rc = -1000; tp01_ready = 1; thread_exit(0); }
    tp01_ready = 1;                                 /* main may now wait for us to block */

    int st = 0;
    tp01_rc = sys_waitpid(-1, &st);                 /* BLOCKS; signal -> want -EINTR */

    /* Still the child's parent: kill it and reap it from our own context. The
     * cleanup waitpid blocks until the child exits (sig_interrupt was already
     * cleared, so it is not spuriously re-interrupted). */
    process_t* c = proc_find(cpid);
    if (c) {
        signal_send(c, SIGKILL);
        int s2 = 0;
        while (sys_waitpid(cpid, &s2) == -EINTR) { thread_yield(); }
    }
    thread_exit(0);
}

KTEST(proc, waitpid_is_interrupted_by_signal) {
    tp01_rc = -4242; tp01_child_pid = 0; tp01_ready = 0;

    process_t* w = thread_create(tp01_parent, 0, "wpwait", PRIO_DEFAULT);
    KASSERT_TEST(w != (void*)0);

    /* Wait until the parent has spawned its child and is actually blocked in
     * waitpid: signal_send only raises EINTR against a thread already BLOCKED. */
    for (int i = 0; i < 600 && !(tp01_ready && w->state == PROC_BLOCKED); i++)
        sched_sleep_ms(5);
    KASSERT_TEST(tp01_child_pid > 0);               /* spinner spawned */
    KASSERT_TEST(w->state == PROC_BLOCKED);         /* parked in waitpid */

    signal_send(w, SIGINT);                         /* interrupt the blocking waitpid */
    thread_join(w, 0);                              /* parent returns, cleans up, exits */

    /* The headline: a signal made a blocked waitpid return -EINTR. Revert the
     * fix (ignore sched_wait_event's return) and the parent re-blocks forever
     * on its still-live child -> thread_join hangs -> suite TIMEOUT. */
    KEXPECT_EQ(tp01_rc, -EINTR);
}
