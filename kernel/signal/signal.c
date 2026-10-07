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
#include <arch/usermode.h>        /* trapframe_t, copy_to/from_user */

/* The context signal_deliver() saves on the user stack and signal_sigreturn()
 * restores. Laid out just above the handler's return address so sigreturn finds
 * it at the user rsp it is entered with. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    uint64_t blocked;             /* sig mask to restore (handler masked its sig) */
} sigcontext_t;

/* Signals whose default action is to terminate the thread. */
static int is_terminate(int sig) {
    switch (sig) {
        case SIGHUP: case SIGINT: case SIGQUIT: case SIGILL:
        case SIGABRT: case SIGKILL: case SIGSEGV: case SIGTERM:
        case SIGPIPE:
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
    if (t->sig_handlers[sig]) return 0;            /* a handler is installed: deliver */
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

/* Called on the way back to ring 3 (from a syscall, or when the timer preempts
 * a user process): if a fatal signal is pending and unblocked, terminate the
 * process with 128 + signo (the shell convention). This is how Ctrl+C actually
 * kills a running user program. Returns normally when there is nothing to do.
 * (User-installed handlers via sigaction/sigreturn are a later brick; here only
 * the default "terminate" action is delivered to a running process.) */
void signal_check_and_die(void) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user) return;
    int sig = signal_take_terminate();
    if (sig) thread_exit(128 + sig);        /* noreturn */
}

int signal_take_terminate(void) {
    process_t* cur = current_process;
    if (!cur) return 0;
    irqflags_t f = local_irq_save();
    uint64_t deliverable = cur->sig_pending & ~cur->sig_blocked;
    cur->sig_interrupt = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & sigmask(sig))) continue;
        /* A signal with a user handler is delivered by signal_deliver() on the
         * syscall path, which can build the frame; leave it pending here. */
        if (cur->sig_handlers[sig]) continue;
        cur->sig_pending &= ~sigmask(sig);          /* accept it */
        if (is_terminate(sig)) { local_irq_restore(f); return sig; }
        /* benign default (ignore): already cleared, keep scanning */
    }
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* User-installed handlers: sigaction / delivery / sigreturn (Phase 20-G)     */
/* -------------------------------------------------------------------------- */

int signal_sigaction(int sig, uint64_t handler, uint64_t restorer) {
    if (sig <= 0 || sig >= NSIG || uncatchable(sig)) return -EINVAL;
    process_t* cur = current_process;
    if (!cur) return -EINVAL;
    irqflags_t f = local_irq_save();
    if (handler == SIG_DFL) {
        cur->sig_handlers[sig] = 0;
        cur->sig_ignore &= ~(1u << sig);
    } else if (handler == SIG_IGN) {
        cur->sig_handlers[sig] = 0;
        cur->sig_ignore |= (1u << sig);
        cur->sig_pending &= ~sigmask(sig);          /* discard already-pending */
    } else {
        cur->sig_handlers[sig] = handler;
        cur->sig_ignore &= ~(1u << sig);
    }
    if (restorer) cur->sig_restorer = restorer;
    local_irq_restore(f);
    return 0;
}

void signal_reset_handlers(process_t* t) {
    if (!t) return;
    for (int s = 0; s < NSIG; s++) t->sig_handlers[s] = 0;
    t->sig_restorer = 0;
    /* SIG_IGN dispositions survive execve (POSIX); sig_ignore is left as is. */
}

/* Build the signal frame on the user stack and point the trapframe at the
 * handler. On a bad user stack the process is killed (SIGSEGV). */
static void deliver_to_handler(process_t* cur, trapframe_t* tf, int sig,
                               uint64_t handler) {
    sigcontext_t sc;
    sc.r15 = tf->r15; sc.r14 = tf->r14; sc.r13 = tf->r13; sc.r12 = tf->r12;
    sc.r11 = tf->r11; sc.r10 = tf->r10; sc.r9 = tf->r9;  sc.r8  = tf->r8;
    sc.rbp = tf->rbp; sc.rdi = tf->rdi; sc.rsi = tf->rsi; sc.rdx = tf->rdx;
    sc.rcx = tf->rcx; sc.rbx = tf->rbx; sc.rax = tf->rax;
    sc.rip = tf->rip; sc.rflags = tf->rflags; sc.rsp = tf->rsp;
    sc.blocked = cur->sig_blocked;

    /* Skip the 128-byte red zone the interrupted frame may be using, then place
     * the context 16-aligned; the return address sits just below it so a plain
     * `ret` from the handler leaves rsp at the context for sigreturn. */
    uint64_t sc_addr = (tf->rsp - 128 - sizeof(sc)) & ~0xFULL;
    uint64_t ret_slot = sc_addr - 8;
    if (!cur->sig_restorer ||
        copy_to_user((void*)(uintptr_t)sc_addr, &sc, sizeof(sc)) < 0 ||
        copy_to_user((void*)(uintptr_t)ret_slot, &cur->sig_restorer, 8) < 0) {
        thread_exit(128 + SIGSEGV);                 /* unusable user stack */
    }

    cur->sig_blocked |= sigmask(sig);               /* mask sig in its handler */
    tf->rsp = ret_slot;
    tf->rip = handler;
    tf->rdi = (uint64_t)sig;                         /* handler(int signo) */
    tf->rax = 0;
}

void signal_deliver(trapframe_t* tf) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user) return;
    irqflags_t f = local_irq_save();
    uint64_t deliverable = cur->sig_pending & ~cur->sig_blocked;
    cur->sig_interrupt = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & sigmask(sig))) continue;
        uint64_t h = cur->sig_handlers[sig];
        cur->sig_pending &= ~sigmask(sig);          /* accept it */
        if (h) {
            local_irq_restore(f);
            deliver_to_handler(cur, tf, sig, h);    /* one handler per return */
            return;
        }
        if (is_terminate(sig)) { local_irq_restore(f); thread_exit(128 + sig); }
        /* benign default (ignore): cleared, keep scanning */
    }
    local_irq_restore(f);
}

long signal_sigreturn(trapframe_t* tf) {
    process_t* cur = current_process;
    sigcontext_t sc;
    if (copy_from_user(&sc, (const void*)(uintptr_t)tf->rsp, sizeof(sc)) < 0)
        thread_exit(128 + SIGSEGV);                 /* forged/clobbered frame */

    tf->r15 = sc.r15; tf->r14 = sc.r14; tf->r13 = sc.r13; tf->r12 = sc.r12;
    tf->r11 = sc.r11; tf->r10 = sc.r10; tf->r9 = sc.r9;  tf->r8  = sc.r8;
    tf->rbp = sc.rbp; tf->rdi = sc.rdi; tf->rsi = sc.rsi; tf->rdx = sc.rdx;
    tf->rcx = sc.rcx; tf->rbx = sc.rbx;
    tf->rip = sc.rip; tf->rsp = sc.rsp;
    /* Sanitise RFLAGS: keep only the user-settable arithmetic/direction flags,
     * force IF=1 and the reserved bit, and never let ring 3 raise IOPL. */
    tf->rflags = (sc.rflags & 0x00000CD5ULL) | 0x202ULL;
    if (cur) cur->sig_blocked = sc.blocked;         /* restore pre-handler mask */
    return (long)sc.rax;                            /* dispatcher sets tf->rax */
}

/* -------------------------------------------------------------------------- */
/* Process groups / sessions                                                  */
/* -------------------------------------------------------------------------- */

int sys_setpgid(int pid, int pgid) {
    process_t* cur = current_process;
    if (!cur) return -ESRCH;
    if (pgid < 0) return -EINVAL;
    process_t* t = (pid == 0) ? cur : proc_find(pid);
    if (!t) return -ESRCH;
    if (t != cur && t->parent_pid != cur->pid) return -ESRCH;  /* self or a child */
    if (t->sid != cur->sid) return -EPERM;                     /* same session   */
    t->pgid = (pgid == 0) ? t->pid : (uint32_t)pgid;           /* 0 => lead group */
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
