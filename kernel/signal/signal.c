/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - signal/signal.c
 * The kernel signal subsystem. See signal.h.
 */

#include <signal.h>
#include <proc_internal.h>
#include <sched.h>
#include <irq.h>
#include <errno.h>

/* Signals whose default action is to terminate the thread. */
static int is_terminate(int sig) {
    switch (sig) {
        case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
        case SIGABRT: case SIGKILL: case SIGSEGV: case SIGTERM:
        case SIGTSTP:   /* no job-control stop yet: treat as terminate */
            return 1;
        default:
            return 0;   /* SIGCHLD, SIGCONT, ... default to ignore */
    }
}

/* SIGKILL and SIGSTOP can never be blocked or ignored. */
static int uncatchable(int sig) { return sig == SIGKILL || sig == SIGSTOP; }

/* A signal is discarded on generation when its effective disposition is to
 * ignore it: either explicitly (set via signal_set_ignore) or because its
 * default action is "ignore" (SIGCHLD, SIGCONT, ...). SIGKILL/SIGSTOP can
 * never be ignored. POSIX: a generated signal with ignore disposition is not
 * added to the pending set — this is what keeps a child's SIGCHLD from piling
 * up on a parent that is not catching it. */
static int effectively_ignored(process_t* t, int sig) {
    if (uncatchable(sig)) return 0;
    if (t->sig_ignore & (1u << sig)) return 1;     /* SIG_IGN disposition */
    return !is_terminate(sig);                     /* SIG_DFL == ignore */
}

int signal_send(process_t* t, int sig) {
    if (!t || sig <= 0 || sig >= NSIG) return -EINVAL;

    irqflags_t f = local_irq_save();
    if (effectively_ignored(t, sig)) {
        local_irq_restore(f);     /* ignored disposition: drop it */
        return 0;
    }
    t->sig_pending |= sigmask(sig);

    /* If the target is blocked in an interruptible wait and this signal is not
     * blocked, wake it so the wait returns EINTR. */
    int deliverable = uncatchable(sig) || !(t->sig_blocked & sigmask(sig));
    if (deliverable && t->state == PROC_BLOCKED) {
        t->sig_interrupt = 1;
        sched_wake(t);
    }
    local_irq_restore(f);
    return 0;
}

int signal_send_pgrp(uint32_t pgid, int sig) {
    int n = 0;
    irqflags_t f = local_irq_save();
    for (process_t* p = all_processes.head; p; p = p->all_next) {
        if (p->pgid == pgid && p->state != PROC_ZOMBIE) {
            /* signal_send takes IRQs off itself; we already hold them, and it
             * is re-entrant on local_irq_save/restore (restore keeps them off
             * while f still says off). To stay simple, set the bit inline. */
            if (effectively_ignored(p, sig)) continue;
            p->sig_pending |= sigmask(sig);
            if ((uncatchable(sig) || !(p->sig_blocked & sigmask(sig))) &&
                p->state == PROC_BLOCKED) {
                p->sig_interrupt = 1;
                sched_wake(p);
            }
            n++;
        }
    }
    local_irq_restore(f);
    return n;
}

int signal_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (pid > 0) {
        process_t* t = proc_find(pid);
        if (!t) return -ESRCH;
        if (sig == 0) return 0;              /* existence check */
        return signal_send(t, sig);
    }
    uint32_t pgid = (pid == 0) ? (current_process ? current_process->pgid : 0)
                               : (uint32_t)(-pid);
    return signal_send_pgrp(pgid, sig) > 0 ? 0 : -ESRCH;
}

int signal_procmask(int how, uint64_t set, uint64_t* oldset) {
    process_t* cur = current_process;
    if (!cur) return -EINVAL;
    irqflags_t f = local_irq_save();
    if (oldset) *oldset = cur->sig_blocked;
    set &= ~(sigmask(SIGKILL) | sigmask(SIGSTOP));   /* never block these */
    switch (how) {
        case SIG_BLOCK:   cur->sig_blocked |= set;  break;
        case SIG_UNBLOCK: cur->sig_blocked &= ~set; break;
        case SIG_SETMASK: cur->sig_blocked = set;   break;
        default: local_irq_restore(f); return -EINVAL;
    }
    local_irq_restore(f);
    return 0;
}

int signal_set_ignore(int sig, int ignore) {
    if (sig <= 0 || sig >= NSIG || uncatchable(sig)) return -EINVAL;
    process_t* cur = current_process;
    if (!cur) return -EINVAL;
    if (ignore) cur->sig_ignore |= (1u << sig);
    else        cur->sig_ignore &= ~(1u << sig);
    return 0;
}

int signal_pending(void) {
    process_t* cur = current_process;
    if (!cur) return 0;
    return (cur->sig_pending & ~cur->sig_blocked) != 0;
}

int signal_take_terminate(void) {
    process_t* cur = current_process;
    if (!cur) return 0;
    irqflags_t f = local_irq_save();
    uint64_t deliverable = cur->sig_pending & ~cur->sig_blocked;
    cur->sig_interrupt = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & sigmask(sig))) continue;
        cur->sig_pending &= ~sigmask(sig);          /* accept it */
        if (is_terminate(sig)) { local_irq_restore(f); return sig; }
        /* benign default (ignore): already cleared, keep scanning */
    }
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Process groups / sessions                                                  */
/* -------------------------------------------------------------------------- */

int sys_setpgid(int pid, int pgid) {
    process_t* t = (pid == 0) ? current_process : proc_find(pid);
    if (!t || pgid < 0) return -ESRCH;
    t->pgid = (pgid == 0) ? t->pid : (uint32_t)pgid;
    return 0;
}

int sys_getpgid(int pid) {
    process_t* t = (pid == 0) ? current_process : proc_find(pid);
    return t ? (int)t->pgid : -ESRCH;
}

int sys_setsid(void) {
    process_t* cur = current_process;
    if (!cur) return -EINVAL;
    cur->sid = cur->pid;        /* new session; becomes its own group leader */
    cur->pgid = cur->pid;
    return (int)cur->sid;
}
