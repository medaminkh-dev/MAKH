/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <sched.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <drivers/timer.h>
#include <arch/usermode.h>
#include <signal.h>
#include <mm/vmspace.h>

/**
 * =============================================================================
 * sched.c - Preemptive priority scheduler
 * =============================================================================
 * Replaces the Phase 9 cooperative round-robin yield with:
 *   - per-priority run queues (0 = highest .. PRIO_IDLE = idle)
 *   - preemption driven by the timer (quantum accounting + need_resched)
 *   - blocking sleep with a tick-ordered wake list
 *   - wait queues (the basis for mutex/cond/join in later phases)
 *   - thread create / exit / join with a reaper for detached threads
 *
 * Single CPU: shared state (run queues, sleep list, wait queues) is protected
 * by clearing IF (local_irq_save/restore). The timer only sets need_resched;
 * the switch happens at the IRQ tail (sched_preempt_if_needed) or when
 * preemption is re-enabled, never in the middle of a scheduler-critical
 * section.
 * =============================================================================
 */

/* From core.c */
extern process_t* current_process;

/* From context_switch.asm */
extern void context_switch(context_t* old, context_t* new);

/* -------------------------------------------------------------------------- */
/* Scheduler state                                                            */
/* -------------------------------------------------------------------------- */

/* One FIFO per priority level (linked via process_t.run_next). */
static process_t* runq_head[PRIO_LEVELS];
static process_t* runq_tail[PRIO_LEVELS];

/* The idle thread, run only when every run queue is empty. */
static process_t* idle_thread = NULL;

/* Threads sleeping until a deadline (unsorted list, linked via sleep_next). */
static process_t* sleep_list = NULL;

/* Detached threads awaiting reaping by the idle thread (via run_next). */
static process_t* reap_list = NULL;

static volatile int need_resched = 0;
/* preempt_count is per-thread (process_t.preempt_count), as in Linux: a
 * global counter would leak to whichever thread runs next if a holder
 * ever blocked. */

/* -------------------------------------------------------------------------- */
/* Run queue helpers (callers hold IRQs disabled)                             */
/* -------------------------------------------------------------------------- */

static void runq_push(process_t* t) {
    uint8_t p = t->priority;
    if (p >= PRIO_LEVELS) p = PRIO_LEVELS - 1;
    t->run_next = NULL;
    if (runq_tail[p]) {
        runq_tail[p]->run_next = t;
        runq_tail[p] = t;
    } else {
        runq_head[p] = runq_tail[p] = t;
    }
}

static process_t* runq_pop_highest(void) {
    for (int p = 0; p < PRIO_LEVELS; p++) {
        process_t* t = runq_head[p];
        if (t) {
            runq_head[p] = t->run_next;
            if (!runq_head[p]) runq_tail[p] = NULL;
            t->run_next = NULL;
            return t;
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Preemption control                                                         */
/* -------------------------------------------------------------------------- */

void preempt_disable(void) {
    irqflags_t f = local_irq_save();
    if (current_process) current_process->preempt_count++;
    local_irq_restore(f);
}

void preempt_enable(void) {
    irqflags_t f = local_irq_save();
    int resched = 0;
    if (current_process) {
        resched = (--current_process->preempt_count == 0) && need_resched;
    }
    local_irq_restore(f);
    if (resched) thread_yield();
}

/* -------------------------------------------------------------------------- */
/* The core switch                                                            */
/* -------------------------------------------------------------------------- */
/*
 * schedule() - pick the next runnable thread and switch to it.
 *
 * Entered with interrupts in any state; runs its critical section with IRQs
 * off. The current thread's state decides whether it is re-queued:
 *   RUNNING -> re-queued as READY (it was preempted or yielded voluntarily)
 *   anything else (BLOCKED/ZOMBIE) -> left off the run queues by its caller.
 */
void schedule(void) {
    irqflags_t flags = local_irq_save();

    process_t* prev = current_process;

    /* Re-queue the outgoing thread FIRST if it is still runnable (never the
     * idle thread - idle lives outside the run queues). It goes to the tail of
     * its own priority level, so equal-priority peers get their turn, but it
     * still competes: a lower-priority thread can never take the CPU from a
     * runnable higher-priority one just because a quantum expired. */
    if (prev && prev != idle_thread && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
        runq_push(prev);
    }

    process_t* next = runq_pop_highest();
    if (!next) {
        /* Nothing runnable at all (prev blocked or exited): run idle. */
        next = idle_thread;
    }

    /* Idle is never queued; just mark it READY when something else runs. */
    if (prev == idle_thread && next != idle_thread) prev->state = PROC_READY;

    /* Stack canary: every heap-allocated thread stack has a magic pattern at
     * its lowest bytes. If a thread overflowed its stack, catch it here, at the
     * first switch after the damage, instead of as random heap corruption. */
    if (prev && prev->stack_canary &&
        *(volatile uint64_t*)prev->kernel_stack != STACK_CANARY_MAGIC) {
        panic("stack overflow in thread '%s' (pid %u): canary smashed",
              prev->name, prev->pid);
    }

    next->state = PROC_RUNNING;
    next->ticks_left = (int64_t)(next->time_slice ? next->time_slice : SCHED_QUANTUM);
    current_process = next;

    if (prev != next) {
        arch_prepare_switch(next);   /* TSS.rsp0 + GS base for the incoming thread */
        context_switch(prev ? &prev->context : NULL, &next->context);
    }

    /* Resumed here (possibly much later). IRQ state is restored by the iretq
     * inside context_switch; this restore covers the no-switch path. */
    local_irq_restore(flags);
}

/* -------------------------------------------------------------------------- */
/* Timer integration                                                          */
/* -------------------------------------------------------------------------- */

/* Unlink t from the global sleep list if present. Caller holds IRQs off. */
static void sleep_remove(process_t* t) {
    process_t** link = &sleep_list;
    while (*link) {
        if (*link == t) { *link = t->sleep_next; t->sleep_next = NULL; return; }
        link = &(*link)->sleep_next;
    }
}

/* Wake any sleepers whose deadline has passed. Caller holds IRQs disabled.
 * A node made READY by another waker (e.g. wq_wake) is just unlinked. */
static void wake_expired_sleepers(uint64_t now) {
    process_t** link = &sleep_list;
    while (*link) {
        process_t* t = *link;
        if (t->state != PROC_BLOCKED) {
            *link = t->sleep_next;      /* stale: already woken elsewhere */
            t->sleep_next = NULL;
            continue;
        }
        if ((int64_t)(now - t->wake_tick) >= 0) {
            *link = t->sleep_next;      /* unlink */
            t->sleep_next = NULL;
            t->timed_out = 1;
            t->state = PROC_READY;
            runq_push(t);
            need_resched = 1;
        } else {
            link = &t->sleep_next;
        }
    }
}

void sched_tick(void) {
    /* Called from the timer IRQ (IRQs already disabled by the gate). */
    wake_expired_sleepers(timer_get_ticks());

    process_t* cur = current_process;
    if (cur && cur != idle_thread) {
        if (--cur->ticks_left <= 0) {
            need_resched = 1;
        }
    } else if (cur == idle_thread) {
        /* Idle should yield the moment anything becomes runnable. */
        for (int p = 0; p < PRIO_LEVELS - 1; p++) {
            if (runq_head[p]) { need_resched = 1; break; }
        }
    }
}

void sched_preempt_if_needed(void) {
    process_t* cur = current_process;
    if (need_resched && (!cur || cur->preempt_count == 0)) {
        need_resched = 0;
        schedule();
    }
}

/* -------------------------------------------------------------------------- */
/* Wake / block                                                               */
/* -------------------------------------------------------------------------- */

void sched_wake(process_t* t) {
    if (!t) return;
    irqflags_t f = local_irq_save();
    if (t->state != PROC_RUNNING && t->state != PROC_READY) {
        t->state = PROC_READY;
        runq_push(t);
        need_resched = 1;
    }
    local_irq_restore(f);
}

void sched_block_current(void) {
    /* Caller has set current_process->state to a non-runnable value. */
    schedule();
}

/* -------------------------------------------------------------------------- */
/* Sleep                                                                      */
/* -------------------------------------------------------------------------- */

void sched_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) { thread_yield(); return; }

    irqflags_t f = local_irq_save();
    process_t* cur = current_process;
    cur->wake_tick = timer_get_ticks() + ticks;
    cur->state = PROC_BLOCKED;
    cur->sleep_next = sleep_list;
    sleep_list = cur;
    schedule();                 /* returns once woken */
    local_irq_restore(f);
}

void sched_sleep_ms(uint64_t ms) {
    /* Timer runs at TIMER_FREQUENCY Hz. Round up so short sleeps aren't zero. */
    uint64_t ticks = (ms * TIMER_FREQUENCY + 999) / 1000;
    sched_sleep_ticks(ticks ? ticks : 1);
}

/* -------------------------------------------------------------------------- */
/* Wait queues                                                                */
/* -------------------------------------------------------------------------- */

void wq_init(wait_queue_t* wq) {
    wq->head = wq->tail = NULL;
}

/* Raw pop of the head node (no state check). Caller holds IRQs off. */
static process_t* wq_pop(wait_queue_t* wq) {
    process_t* t = wq->head;
    if (t) {
        wq->head = t->wq_next;
        if (!wq->head) wq->tail = NULL;
        t->wq_next = NULL;
    }
    return t;
}

/* Unlink t from wq if present. Caller holds IRQs off. */
static void wq_remove(wait_queue_t* wq, process_t* t) {
    process_t** link = &wq->head;
    process_t* prev = NULL;
    while (*link) {
        if (*link == t) {
            *link = t->wq_next;
            if (wq->tail == t) wq->tail = prev;
            t->wq_next = NULL;
            return;
        }
        prev = *link;
        link = &(*link)->wq_next;
    }
}

/*
 * Block the current thread on wq, optionally with a timeout (in ticks; 0 = no
 * timeout). Returns 0 if woken by wq_wake*, or 1 if the timeout expired. The
 * caller must hold IRQs disabled (flags from local_irq_save); IRQs are restored
 * to that state on return. The node is placed on wq and, if a timeout is set,
 * also on the sleep list; whichever waker fires first sets the state to READY,
 * and this routine unlinks the node from both lists on wake.
 */
int sched_wait_event(wait_queue_t* wq, uint64_t timeout_ticks, irqflags_t flags) {
    process_t* cur = current_process;
    cur->state = PROC_BLOCKED;
    cur->timed_out = 0;

    cur->wq_next = NULL;
    if (wq->tail) { wq->tail->wq_next = cur; wq->tail = cur; }
    else          { wq->head = wq->tail = cur; }

    if (timeout_ticks) {
        cur->wake_tick = timer_get_ticks() + timeout_ticks;
        cur->sleep_next = sleep_list;
        sleep_list = cur;
    }

    schedule();                 /* sleeps here until woken by wq or timer */

    /* Woken. Ensure we are off both lists (the waker unlinked one of them). */
    local_irq_save();
    wq_remove(wq, cur);
    sleep_remove(cur);
    int timed = cur->timed_out;
    int intr = cur->sig_interrupt;      /* Phase 19: a signal woke us */
    cur->sig_interrupt = 0;
    local_irq_restore(flags);
    if (intr) return 2;                 /* EINTR: caller should re-check signals */
    return timed;
}

void wq_block(wait_queue_t* wq, irqflags_t flags) {
    (void)sched_wait_event(wq, 0, flags);
}

void wq_wake_one(wait_queue_t* wq) {
    irqflags_t f = local_irq_save();
    process_t* t;
    while ((t = wq_pop(wq)) != NULL) {
        if (t->state != PROC_BLOCKED) continue;   /* stale (timed out); drop */
        t->timed_out = 0;
        t->state = PROC_READY;
        runq_push(t);
        need_resched = 1;
        break;
    }
    local_irq_restore(f);
}

void wq_wake_all(wait_queue_t* wq) {
    irqflags_t f = local_irq_save();
    process_t* t;
    while ((t = wq_pop(wq)) != NULL) {
        if (t->state != PROC_BLOCKED) continue;
        t->timed_out = 0;
        t->state = PROC_READY;
        runq_push(t);
        need_resched = 1;
    }
    local_irq_restore(f);
}

/* -------------------------------------------------------------------------- */
/* Thread creation                                                            */
/* -------------------------------------------------------------------------- */

/* Trampoline: first thing a new thread runs. Calls entry(arg), then exits. */
static void thread_trampoline(void) {
    process_t* self = current_process;
    thread_entry_t entry = (thread_entry_t)self->entry;
    void* arg = self->entry_arg;
    if (entry) entry(arg);
    thread_exit(0);
}

process_t* thread_create(thread_entry_t entry, void* arg,
                         const char* name, uint8_t priority) {
    process_t* t = proc_table_alloc();
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    void* stack = kmalloc(DEFAULT_THREAD_STACK);
    if (!stack) { proc_table_free(t); return NULL; }
    memset(stack, 0, DEFAULT_THREAD_STACK);
    /* Stack grows down: the lowest qword is the last thing an overflow hits
     * before it tramples a neighbouring heap block. schedule() checks it. */
    *(volatile uint64_t*)stack = STACK_CANARY_MAGIC;

    uint32_t pid = pid_alloc();
    if (pid == 0) { kfree(stack); proc_table_free(t); return NULL; }

    t->pid = pid;
    t->pgid = pid; t->sid = pid;   /* Phase 19: own group/session by default */
    t->state = PROC_EMBRYO;
    t->priority = priority < PRIO_LEVELS ? priority : PRIO_DEFAULT;
    t->kernel_stack = (uint64_t)stack;
    t->kernel_stack_size = DEFAULT_THREAD_STACK;
    t->stack_canary = 1;
    t->time_slice = SCHED_QUANTUM;
    t->ticks_left = SCHED_QUANTUM;
    t->entry = entry;
    t->entry_arg = arg;

    if (name) {
        int i = 0;
        for (; i < 31 && name[i]; i++) t->name[i] = name[i];
        t->name[i] = '\0';
    }

    /* Build the initial kernel stack: 16-byte aligned, with a return slot so
     * the ABI's "return address at [rsp]" invariant holds when the trampoline
     * (unexpectedly) returns. */
    uint64_t top = (t->kernel_stack + t->kernel_stack_size) & ~(uint64_t)0xF;
    top -= 8;
    *(uint64_t*)top = (uint64_t)(uintptr_t)thread_exit;  /* safety net */

    t->context.rsp = top;
    t->context.rip = (uint64_t)(uintptr_t)thread_trampoline;
    t->context.rflags = 0x202;   /* IF=1 */
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->context.cr3));
    t->context.cs = 0x08;
    t->context.ds = t->context.es = t->context.fs = t->context.gs = t->context.ss = 0x10;

    t->creation_time = timer_get_ticks();

    /* Link into the global process list and the parent's child list. */
    irqflags_t f = local_irq_save();
    all_list_add(t);
    if (current_process) {
        t->parent_pid = current_process->pid;
        proc_add_child(current_process, t);
    }
    t->state = PROC_READY;
    runq_push(t);
    need_resched = 1;
    local_irq_restore(f);

    return t;
}

/* -------------------------------------------------------------------------- */
/* Yield / exit / join                                                        */
/* -------------------------------------------------------------------------- */

void thread_yield(void) {
    schedule();
}

/* Legacy Phase 9 names still used by boot-time demos. */
void proc_yield(void) { schedule(); }
void proc_add_to_ready(process_t* t) { sched_wake(t); }

static void reap(process_t* t) {
    /* Caller holds IRQs disabled. Frees everything owned by a dead thread. */
    all_list_remove(t);
    if (t->aspace) {            /* Phase 20-A: user process address space */
        vmspace_destroy((address_space_t*)t->aspace);
        kfree(t->aspace);
        t->aspace = NULL;
    }
    if (t->tls) {
        kfree(t->tls);
        t->tls = NULL;
    }
    if (t->kernel_stack) {
        kfree((void*)t->kernel_stack);
        t->kernel_stack = 0;
    }
    pid_free(t->pid);
    t->reaped = 1;
    proc_table_free(t);
}

/* Public reap for waitpid(): reap an already-ZOMBIE child under IRQs off. */
void proc_reap(process_t* t) {
    if (!t) return;
    irqflags_t f = local_irq_save();
    if (!t->reaped) reap(t);
    local_irq_restore(f);
}

/* Free any detached threads that have exited. Called by the idle thread, which
 * has its own (static) stack and so can safely free theirs. */
void sched_reap_detached(void) {
    for (;;) {
        /* reap() edits the global process list, PID bitmap and process table,
         * so it must run with IRQs off: the idle thread is preemptible, and a
         * preemption mid-unlink (e.g. into a thread calling thread_create)
         * used to corrupt all_processes - found by the net stress tests. */
        irqflags_t f = local_irq_save();
        process_t* t = reap_list;
        if (t) {
            reap_list = t->run_next;
            reap(t);
        }
        local_irq_restore(f);
        if (!t) break;
    }
}

/* Defined in kernel/pthread/pthread.c; runs per-thread key destructors. */
extern void pthread_tls_cleanup(process_t* t);

void thread_exit(int code) {
    /* Run TLS destructors in thread context (IRQs on) before we tear down. */
    pthread_tls_cleanup(current_process);

    irqflags_t f = local_irq_save();
    process_t* cur = current_process;

    cur->exit_code = code;
    cur->exit_time = timer_get_ticks();
    cur->cpu_time_used += (cur->exit_time - cur->creation_time);

    /* Give our children to init (PID 1) so nobody is orphaned. */
    proc_reparent_orphans(cur);
    if (cur->parent_pid != 0 && cur->parent_pid != cur->pid) {
        proc_remove_child(cur);
    }

    cur->state = PROC_ZOMBIE;

    /* Wake any thread blocked in thread_join() on us; it will reap us. */
    wq_wake_all(&cur->join_wq);

    /* Phase 20-A: notify a parent blocked in waitpid(), and raise SIGCHLD. */
    process_t* parent = proc_find(cur->parent_pid);
    if (parent && parent != cur) {
        wq_wake_all(&parent->child_wq);
        signal_send(parent, SIGCHLD);
    }

    if (cur->detached) {
        /* No joiner will ever come: hand ourselves to the idle reaper. */
        cur->run_next = reap_list;
        reap_list = cur;
    }
    /* Otherwise (joinable) we stay a ZOMBIE until someone joins us. */

    schedule();                 /* never returns */
    local_irq_restore(f);       /* unreachable */
    for (;;) __asm__ volatile("hlt");
}

int sched_join(process_t* t, int* code, void** retval) {
    if (!t) return -1;
    irqflags_t f = local_irq_save();
    if (t->reaped || t->detached) { local_irq_restore(f); return -1; }

    while (t->state != PROC_ZOMBIE) {
        wq_block(&t->join_wq, f);  /* releases IRQs, sleeps, re-enters below */
        f = local_irq_save();
    }

    if (code)   *code = t->exit_code;
    if (retval) *retval = t->retval;
    reap(t);
    local_irq_restore(f);
    return 0;
}

int thread_join(process_t* t, int* code) {
    return sched_join(t, code, NULL);
}

int sched_detach(process_t* t) {
    if (!t) return -1;
    irqflags_t f = local_irq_save();
    if (t->reaped) { local_irq_restore(f); return -1; }
    t->detached = 1;
    if (t->state == PROC_ZOMBIE) {
        /* Already exited with no joiner: reap it now. */
        reap(t);
    }
    local_irq_restore(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Idle thread                                                                */
/* -------------------------------------------------------------------------- */

void sched_set_idle(process_t* t) { idle_thread = t; }

void sched_idle_loop(void) {
    for (;;) {
        sched_reap_detached();
        __asm__ volatile("sti; hlt");
    }
}

void sched_init(void) {
    for (int i = 0; i < PRIO_LEVELS; i++) {
        runq_head[i] = runq_tail[i] = NULL;
    }
    sleep_list = NULL;
    reap_list = NULL;
    need_resched = 0;
}

/* Debug: print every thread (used by tests / the shell). */
void sched_debug_dump(void) {
    static const char* st[] = { "EMBRYO", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };
    uint64_t now = timer_get_ticks();
    kprintf("  PID  PPID PRIO STATE    AGE(ticks) NAME\n");
    irqflags_t f = local_irq_save();
    for (process_t* p = all_processes.head; p; p = p->all_next) {
        kprintf("%5u %5u %4u %-8s %10lu %s%s\n", p->pid, p->parent_pid, p->priority,
                st[p->state], (unsigned long)(now - p->creation_time), p->name,
                p == current_process ? " *" : "");
    }
    local_irq_restore(f);
}
