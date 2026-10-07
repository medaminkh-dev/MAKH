/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_SCHED_H
#define MAKHOS_SCHED_H

#include <types.h>
#include <proc.h>
#include <irq.h>

/**
 * =============================================================================
 * sched.h - Preemptive priority scheduler + synchronization primitives
 * =============================================================================
 * MakhOS runs entirely in ring 0 for now, so a "thread" is a kernel context
 * with its own stack that shares the single address space. process_t is that
 * context (see proc.h). This header layers a preemptive, priority-based
 * scheduler and the blocking primitives (wait queues, sleep) that the POSIX
 * threads API (Phase 13) and the self-fuzzer (Phase 15) are built on.
 *
 * Concurrency model (single CPU): the scheduler's queues and the sleep list
 * are shared with the timer IRQ, so they are protected by disabling
 * interrupts (irq_save/irq_restore). Preemption is deferred while
 * preempt_count > 0; the timer only sets need_resched, and the actual switch
 * happens at the IRQ tail or when preemption is re-enabled.
 * =============================================================================
 */

/* Priority levels: 0 = highest, PRIO_LEVELS-1 = lowest (idle uses the lowest). */
#define PRIO_LEVELS      32
#define PRIO_DEFAULT     16
#define PRIO_IDLE        (PRIO_LEVELS - 1)

/* Default scheduling quantum, in timer ticks (timer runs at 100Hz => 10ms). */
#define SCHED_QUANTUM    5

/* -------- interrupt / preemption control --------
 * local_irq_save/restore live in irq.h (included above). */

void preempt_disable(void);
void preempt_enable(void);

/* -------- scheduler lifecycle -------- */

void sched_init(void);                 /* set up run queues (called by proc_init) */
void schedule(void);                   /* pick + switch to the next runnable thread */
void sched_tick(void);                 /* accounting + preemption decision (from timer IRQ) */
void sched_preempt_if_needed(void);    /* run a deferred reschedule (from IRQ tail) */

/* Make `t` runnable and enqueue it at its priority. */
void sched_wake(process_t* t);
/* Block the current thread (state must already be set to BLOCKED/etc.). */
void sched_block_current(void);

/* -------- threads -------- */

typedef void (*thread_entry_t)(void* arg);

/* Create a runnable kernel thread. Returns the PCB or NULL. */
process_t* thread_create(thread_entry_t entry, void* arg,
                         const char* name, uint8_t priority);

/* Admit a fully-built PCB to the run queue (Phase 20-A-2: fork). The caller
 * must have set everything — context, address space, lists — first; this is
 * the single step that makes the thread schedulable, so there is no window in
 * which it could run half-formed. */
void sched_admit(process_t* t);

void thread_yield(void);
void thread_exit(int code) __attribute__((noreturn));

/* Wait for `t` to exit; stores its exit code in *code (if non-NULL) and reaps
 * it. Returns 0 on success, -1 if t is invalid or already reaped. */
int  thread_join(process_t* t, int* code);
/* Same, also returning the thread's void* return value (for pthread_join). */
int  sched_join(process_t* t, int* code, void** retval);
/* Mark a thread detached; reap immediately if it has already exited. */
int  sched_detach(process_t* t);

/* Sleep the current thread for at least the given time. */
void sched_sleep_ticks(uint64_t ticks);
void sched_sleep_ms(uint64_t ms);

/* -------- idle thread (set up by proc_init) -------- */

void sched_set_idle(process_t* t);
void sched_idle_loop(void) __attribute__((noreturn));
void sched_reap_detached(void);

/* -------- wait queues (blocking synchronization foundation) --------
 * wait_queue_t is defined in proc.h so it can be embedded in the PCB. */

void wq_init(wait_queue_t* wq);
/* Block current thread on wq. Must be called with IRQs about to be released;
 * pass the flags from local_irq_save() so the enqueue+switch is atomic. */
void wq_block(wait_queue_t* wq, irqflags_t flags);
/* Block on wq with an optional timeout (ticks; 0 = forever). Returns 0 if woken
 * by wq_wake*, 1 if the timeout expired. Caller holds IRQs off (flags). */
int  sched_wait_event(wait_queue_t* wq, uint64_t timeout_ticks, irqflags_t flags);
void wq_wake_one(wait_queue_t* wq);
void wq_wake_all(wait_queue_t* wq);

void sched_debug_dump(void);   /* print every thread (for `ps`) */

#endif /* MAKHOS_SCHED_H */
