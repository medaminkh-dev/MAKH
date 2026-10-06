/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <sched.h>
#include <mm/kheap.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <lib/string.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * core.c - Process manager bootstrap
 * =============================================================================
 * Sets up the two special threads that always exist:
 *   PID 0 (idle) - runs sched_idle_loop(): reaps detached threads, then hlt.
 *                  Never on a run queue; scheduled only when nothing else is.
 *   PID 1 (init) - the context kernel_main is already running on. We adopt it
 *                  as the current thread so the first schedule() can save it.
 * =============================================================================
 */

/* Globals shared with the scheduler and other proc modules. */
process_t *current_process = NULL;
process_list_t all_processes;

/* Static PCBs and stacks for the two bootstrap threads. */
uint8_t idle_stack[IDLE_STACK_SIZE] __attribute__((aligned(16)));
uint8_t init_stack[INIT_STACK_SIZE] __attribute__((aligned(16)));

static process_t idle_pcb;
static process_t init_pcb;

void phex(uint64_t v) {
    kprintf("%x", v);
}

static void init_common(process_t* p, uint32_t pid, const char* name,
                        uint8_t prio, uint8_t* stack, uint64_t stack_size) {
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->pgid = pid; p->sid = pid;
    p->state = PROC_READY;
    p->priority = prio;
    p->kernel_stack = (uint64_t)stack;
    p->kernel_stack_size = stack_size;
    p->time_slice = SCHED_QUANTUM;
    p->ticks_left = SCHED_QUANTUM;

    int i = 0;
    for (; name[i] && i < 31; i++) p->name[i] = name[i];
    p->name[i] = '\0';

    p->context.rflags = 0x202;
    __asm__ volatile("mov %%cr3, %0" : "=r"(p->context.cr3));
    p->context.cs = 0x08;
    p->context.ds = p->context.es = p->context.fs = p->context.gs = p->context.ss = 0x10;

    p->creation_time = timer_get_ticks();
}

void proc_init(void) {
    KLOG_I("PROC", "initializing process manager (preemptive)\n");

    pid_init();
    proc_table_init();
    sched_init();
    memset(&all_processes, 0, sizeof(all_processes));

    memset(idle_stack, 0, IDLE_STACK_SIZE);
    memset(init_stack, 0, INIT_STACK_SIZE);

    /* ---- Idle (PID 0) ---- */
    init_common(&idle_pcb, 0, "idle", PRIO_IDLE, idle_stack, IDLE_STACK_SIZE);
    uint64_t itop = ((uint64_t)idle_stack + IDLE_STACK_SIZE) & ~(uint64_t)0xF;
    itop -= 8;
    *(uint64_t*)itop = 0;
    idle_pcb.context.rsp = itop;
    idle_pcb.context.rip = (uint64_t)(uintptr_t)sched_idle_loop;
    all_list_add(&idle_pcb);
    proc_table_insert(&idle_pcb);
    sched_set_idle(&idle_pcb);

    /* ---- Init (PID 1) = the thread kernel_main runs on ---- */
    init_common(&init_pcb, 1, "init", PRIO_DEFAULT, init_stack, INIT_STACK_SIZE);
    all_list_add(&init_pcb);
    proc_table_insert(&init_pcb);   /* proc_find(1) must work: see table.c */

    KLOG_I("PROC", "idle (PID 0) and init (PID 1) created\n");
}

process_t *proc_current(void) {
    return current_process;
}

uint32_t proc_get_pid(void) {
    return current_process ? current_process->pid : 0;
}

void proc_become_current(void) {
    /* Adopt the current CPU context (kernel_main) as PID 1 (init). The first
     * schedule() will save our live register state into init_pcb. */
    init_pcb.state = PROC_RUNNING;
    current_process = &init_pcb;
    KLOG_I("PROC", "kernel_main is now init (PID 1)\n");
}
