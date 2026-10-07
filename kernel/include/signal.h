/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - signal.h
 * POSIX-style signals (Phase 19).
 *
 * Each thread carries a pending set, a blocked mask (sigprocmask) and a
 * per-signal disposition (default or ignore). kill() / signal_send() set a
 * pending bit and, if the target is blocked in an interruptible wait, wake it
 * so the wait returns EINTR. A thread acts on its signals at safe points by
 * calling signal_take_terminate(): the default action for the fatal signals is
 * to terminate, which is how Ctrl+C (SIGINT to the terminal's foreground
 * process group) kills a job.
 *
 * Running a user-installed handler and sigreturn in ring 3 is Phase 20 (it
 * needs the persistent user-process model); this phase is the kernel subsystem
 * and its default actions, which the two acceptance tests exercise.
 */

#ifndef MAKHOS_SIGNAL_H
#define MAKHOS_SIGNAL_H

#include <types.h>

#define NSIG        32

#define SIGHUP      1
#define SIGINT      2       /* Ctrl+C          */
#define SIGQUIT     3
#define SIGILL      4
#define SIGABRT     6
#define SIGKILL     9       /* uncatchable     */
#define SIGSEGV     11      /* bad memory access */
#define SIGTERM     15
#define SIGCHLD     17      /* child stopped/exited (default: ignore) */
#define SIGCONT     18      /* continue (default: ignore)            */
#define SIGSTOP     19      /* uncatchable stop                      */
#define SIGTSTP     20      /* Ctrl+Z                                */

/* sigprocmask how. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* Special handler values for sigaction (Phase 20-G). */
#define SIG_DFL      0      /* default action                 */
#define SIG_IGN      1      /* ignore                         */

#define sigmask(s)  (1ull << (s))

struct process;
struct trapframe;

/* Deliver `sig` to one thread. Returns 0, or -errno. */
int  signal_send(struct process* t, int sig);
/* Deliver `sig` to every thread in process group `pgid`. Returns the count. */
int  signal_send_pgrp(uint32_t pgid, int sig);
/* kill(2): pid > 0 -> that thread; pid == 0 -> caller's group; pid < 0 -> group -pid. */
int  signal_kill(int pid, int sig);

/* sigprocmask(2) on the current thread. */
int  signal_procmask(int how, uint64_t set, uint64_t* oldset);

/* Set a signal's disposition to ignore (1) or default (0). */
int  signal_set_ignore(int sig, int ignore);

/* True if the current thread has an unblocked pending signal. */
int  signal_pending(void);

/*
 * If the current thread has a pending, unblocked signal whose default action
 * is to terminate, clear it and return the signal number; otherwise handle any
 * ign(ored)/benign pending signals and return 0. A thread calls this at a safe
 * point and, on a non-zero return, exits with code 128 + signo.
 */
int  signal_take_terminate(void);

/* Return-to-ring-3 hook: terminate the current user process (128+signo) if it
 * has a pending fatal signal with NO user handler. Called from the timer-preempt
 * (IRQ) path, which cannot build a handler frame; handler'd signals are left
 * pending for signal_deliver() at the next syscall return. */
void signal_check_and_die(void);

/* Full return-to-ring-3 delivery on the syscall path (has the trapframe): if a
 * signal with a user handler is pending, build a signal frame on the user stack
 * and redirect the trapframe into the handler; otherwise apply the default
 * action (terminate or ignore). (Phase 20-G.) */
void signal_deliver(struct trapframe* tf);

/* sigaction(2): install handler (SIG_DFL/SIG_IGN or a user address) for sig,
 * with the user trampoline (sa_restorer) that calls sigreturn. */
int  signal_sigaction(int sig, uint64_t handler, uint64_t restorer);

/* sigreturn(2): restore the context saved by signal_deliver from the user
 * stack. Returns the restored rax (the dispatcher writes it into tf->rax). */
long signal_sigreturn(struct trapframe* tf);

/* Reset caught handlers to SIG_DFL across execve (SIG_IGN dispositions stay). */
void signal_reset_handlers(struct process* t);

/* Process-group / session calls. */
int  sys_setpgid(int pid, int pgid);
int  sys_getpgid(int pid);
int  sys_setsid(void);

#endif /* MAKHOS_SIGNAL_H */
