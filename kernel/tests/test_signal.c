/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_signal.c
 * Phase 19: signals, process groups and the TTY line discipline. The headline
 * tests prove "Ctrl+C kills a process" (via the terminal's foreground process
 * group) and job control (background groups are spared).
 */

#include <ktest.h>
#include <signal.h>
#include <tty.h>
#include <sched.h>
#include <proc.h>
#include <proc_internal.h>
#include <irq.h>
#include <lib/string.h>

/* A worker that runs until a terminating signal arrives, then exits 128+signo
 * (the shell's convention). */
static void sig_worker(void* arg) {
    (void)arg;
    int s;
    while ((s = signal_take_terminate()) == 0) thread_yield();
    thread_exit(128 + s);
}

KTEST(signal, kill_terminates_a_thread) {
    process_t* w = thread_create(sig_worker, 0, "killw", PRIO_DEFAULT);
    KASSERT_TEST(w != (void*)0);
    KEXPECT_EQ(signal_send(w, SIGKILL), 0);
    int code = -1;
    KEXPECT_EQ(thread_join(w, &code), 0);
    KEXPECT_EQ(code, 128 + SIGKILL);
}

KTEST(signal, ctrl_c_kills_foreground_pgroup_only) {
    enum { N = 3 };
    process_t* fg[N];
    uint32_t group = 0;
    for (int i = 0; i < N; i++) {
        fg[i] = thread_create(sig_worker, 0, "fg", PRIO_DEFAULT);
        KASSERT_TEST(fg[i] != (void*)0);
        if (i == 0) group = fg[0]->pid;
        fg[i]->pgid = group;                    /* all in one process group */
    }
    /* A background job in its own group must survive Ctrl+C. */
    process_t* bg = thread_create(sig_worker, 0, "bg", PRIO_DEFAULT);
    KASSERT_TEST(bg != (void*)0);               /* bg->pgid == bg->pid (its own) */

    tty_set_foreground(group);
    tty_input(3);                               /* ^C -> SIGINT to the fg group */

    for (int i = 0; i < N; i++) {
        int code = -1;
        KEXPECT_EQ(thread_join(fg[i], &code), 0);
        KEXPECT_EQ(code, 128 + SIGINT);         /* killed by Ctrl+C */
    }
    KEXPECT(bg->state != PROC_ZOMBIE);          /* background job spared */

    signal_send(bg, SIGKILL);                   /* clean up */
    int code; thread_join(bg, &code);
    KEXPECT_EQ(code, 128 + SIGKILL);
}

KTEST(signal, blocked_signal_stays_pending) {
    uint64_t old = 0;
    signal_procmask(SIG_BLOCK, sigmask(SIGINT), &old);
    signal_send(current_process, SIGINT);
    KEXPECT_EQ(signal_take_terminate(), 0);     /* blocked -> not delivered */
    KEXPECT_EQ(signal_pending(), 0);            /* pending, but masked */
    signal_procmask(SIG_UNBLOCK, sigmask(SIGINT), 0);
    KEXPECT(signal_pending() != 0);             /* now deliverable */
    KEXPECT_EQ(signal_take_terminate(), SIGINT);
    signal_procmask(SIG_SETMASK, old, 0);       /* restore */
}

KTEST(signal, ignored_signal_is_dropped) {
    signal_set_ignore(SIGINT, 1);
    signal_send(current_process, SIGINT);
    KEXPECT_EQ(signal_pending(), 0);            /* ignored on arrival */
    KEXPECT_EQ(signal_take_terminate(), 0);
    signal_set_ignore(SIGINT, 0);
}

static wait_queue_t eintr_wq;
static volatile int eintr_rc;

static void eintr_worker(void* arg) {
    (void)arg;
    irqflags_t f = local_irq_save();
    eintr_rc = sched_wait_event(&eintr_wq, 0, f);   /* block forever */
    thread_exit(0);
}

KTEST(signal, signal_interrupts_blocking_wait_eintr) {
    wq_init(&eintr_wq);
    eintr_rc = -99;
    process_t* w = thread_create(eintr_worker, 0, "eintr", PRIO_DEFAULT);
    KASSERT_TEST(w != (void*)0);
    sched_sleep_ms(20);                         /* let it reach the wait */
    KEXPECT_EQ(w->state, PROC_BLOCKED);
    signal_send(w, SIGINT);                     /* wakes it with EINTR */
    thread_join(w, 0);
    KEXPECT_EQ(eintr_rc, 2);                    /* sched_wait_event returned EINTR */
}

KTEST(signal, process_groups_and_sessions) {
    uint32_t mypid = current_process->pid;
    KEXPECT_EQ(sys_getpgid(0), (int)current_process->pgid);
    KEXPECT_EQ(sys_setpgid(0, (int)mypid), 0);
    KEXPECT_EQ(sys_getpgid(0), (int)mypid);
    int sid = sys_setsid();
    KEXPECT_EQ(sid, (int)mypid);
    KEXPECT_EQ((int)current_process->pgid, (int)mypid);
}

/* -------------------------------------------------------------------------- */
/* TTY line discipline                                                        */
/* -------------------------------------------------------------------------- */

KTEST(tty, canonical_line_editing) {
    tty_init();                                 /* isolate from the fuzzer's leftovers */
    tty_set_foreground(0xFFFFFF);               /* no real group: ^C is harmless */
    tty_termios()->c_lflag = ICANON | ECHO | ISIG;
    tty_input('a'); tty_input('b'); tty_input('\b'); tty_input('c'); tty_input('\n');
    KEXPECT(tty_line_ready());
    char buf[8]; memset(buf, 0, sizeof(buf));
    long n = tty_read(buf, sizeof(buf));
    KEXPECT_EQ(n, 3);                           /* "ac\n" (b was erased) */
    KEXPECT_EQ(memcmp(buf, "ac\n", 3), 0);
}

KTEST(tty, eof_returns_zero) {
    tty_init();                                 /* fresh, empty line buffer */
    char buf[8];
    tty_input(4);                               /* ^D on an empty line -> EOF */
    KEXPECT_EQ(tty_read(buf, sizeof(buf)), 0);
}
