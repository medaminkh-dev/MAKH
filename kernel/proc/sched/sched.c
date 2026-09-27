#include <proc_internal.h>
#include <sched.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <lib/string.h>
#include <mm/kheap.h>
#include <drivers/timer.h>

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
static volatile int preempt_count = 0;

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
    preempt_count++;
    local_irq_restore(f);
}

void preempt_enable(void) {
    irqflags_t f = local_irq_save();
    int resched = (--preempt_count == 0) && need_resched;
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
    process_t* next = runq_pop_highest();

    if (!next) {
        /* Nothing else ready. If the caller is still runnable, keep running
         * it; otherwise fall back to the idle thread. */
        if (prev && prev->state == PROC_RUNNING) {
            local_irq_restore(flags);
            return;
        }
        next = idle_thread;
    }

    /* Re-queue the outgoing thread if it is still runnable (never the idle
     * thread - idle lives outside the run queues). */
    if (prev && prev != idle_thread && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
        runq_push(prev);
    }

    next->state = PROC_RUNNING;
    next->ticks_left = (int64_t)(next->time_slice ? next->time_slice : SCHED_QUANTUM);
    current_process = next;

    if (prev != next) {
        context_switch(prev ? &prev->context : NULL, &next->context);
    }

    /* Resumed here (possibly much later). IRQ state is restored by the iretq
     * inside context_switch; this restore covers the no-switch path. */
    local_irq_restore(flags);
}

/* -------------------------------------------------------------------------- */
/* Timer integration                                                          */
/* -------------------------------------------------------------------------- */

/* Wake any sleepers whose deadline has passed. Caller holds IRQs disabled. */
static void wake_expired_sleepers(uint64_t now) {
    process_t** link = &sleep_list;
    while (*link) {
        process_t* t = *link;
        if ((int64_t)(now - t->wake_tick) >= 0) {
            *link = t->sleep_next;      /* unlink */
            t->sleep_next = NULL;
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
    if (preempt_count == 0 && need_resched) {
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

void wq_block(wait_queue_t* wq, irqflags_t flags) {
    /* Called with IRQs already disabled (flags captured by the caller). */
    process_t* cur = current_process;
    cur->state = PROC_BLOCKED;
    cur->wq_next = NULL;
    if (wq->tail) { wq->tail->wq_next = cur; wq->tail = cur; }
    else          { wq->head = wq->tail = cur; }

    schedule();                 /* sleeps here until woken */
    local_irq_restore(flags);
}

static process_t* wq_dequeue(wait_queue_t* wq) {
    process_t* t = wq->head;
    if (t) {
        wq->head = t->wq_next;
        if (!wq->head) wq->tail = NULL;
        t->wq_next = NULL;
    }
    return t;
}

void wq_wake_one(wait_queue_t* wq) {
    irqflags_t f = local_irq_save();
    process_t* t = wq_dequeue(wq);
    if (t) {
        t->state = PROC_READY;
        runq_push(t);
        need_resched = 1;
    }
    local_irq_restore(f);
}

void wq_wake_all(wait_queue_t* wq) {
    irqflags_t f = local_irq_save();
    process_t* t;
    while ((t = wq_dequeue(wq)) != NULL) {
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

    uint32_t pid = pid_alloc();
    if (pid == 0) { kfree(stack); proc_table_free(t); return NULL; }

    t->pid = pid;
    t->state = PROC_EMBRYO;
    t->priority = priority < PRIO_LEVELS ? priority : PRIO_DEFAULT;
    t->kernel_stack = (uint64_t)stack;
    t->kernel_stack_size = DEFAULT_THREAD_STACK;
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
    if (t->kernel_stack) {
        kfree((void*)t->kernel_stack);
        t->kernel_stack = 0;
    }
    pid_free(t->pid);
    t->reaped = 1;
    proc_table_free(t);
}

/* Free any detached threads that have exited. Called by the idle thread, which
 * has its own (static) stack and so can safely free theirs. */
void sched_reap_detached(void) {
    for (;;) {
        irqflags_t f = local_irq_save();
        process_t* t = reap_list;
        if (t) reap_list = t->run_next;
        local_irq_restore(f);
        if (!t) break;
        reap(t);
    }
}

void thread_exit(int code) {
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

int thread_join(process_t* t, int* code) {
    if (!t) return -1;
    irqflags_t f = local_irq_save();
    if (t->reaped || t->detached) { local_irq_restore(f); return -1; }

    while (t->state != PROC_ZOMBIE) {
        wq_block(&t->join_wq, f);  /* releases IRQs, sleeps, re-enters below */
        f = local_irq_save();
    }

    if (code) *code = t->exit_code;
    reap(t);
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
    preempt_count = 0;
}
