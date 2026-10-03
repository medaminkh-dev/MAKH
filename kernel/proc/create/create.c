/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <sched.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <lib/string.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * create.c - Process Creation
 * =============================================================================
 * Creates new processes with proper context and stack setup.
 * Phase 11: Added parent-child relationships and statistics.
 * =============================================================================
 */

#define DEFAULT_STACK_SIZE  8192
/* Phase 12: priorities are 0 (highest) .. PRIO_IDLE; the old value 128 was
 * clamped to the idle level and starved these threads. */
#define DEFAULT_PRIORITY    PRIO_DEFAULT
#define DEFAULT_RFLAGS      0x202

process_t *proc_create(void (*entry)(void), uint64_t stack_size, const char* name) {
    terminal_writestring("[PROC] Creating new process...\n");

    // Validate stack size
    if (stack_size < MIN_STACK_SIZE)
        stack_size = DEFAULT_STACK_SIZE;

    // Allocate from process table
    process_t *proc = proc_table_alloc();
    if (!proc) {
        terminal_writestring("[PROC] ERROR: Failed to allocate process table slot\n");
        return NULL;
    }
    
    // Clear PCB (already zero from kmalloc in table.c, but ensure)
    memset(proc, 0, sizeof(process_t));

    // Allocate kernel stack
    void *stack = kmalloc(stack_size);
    if (!stack) {
        terminal_writestring("[PROC] ERROR: kmalloc stack failed\n");
        proc_table_free(proc);
        return NULL;
    }
    memset(stack, 0, stack_size);

    // Assign PID
    uint32_t pid = pid_alloc();

    if (pid == 0) {
        terminal_writestring("[PROC] ERROR: Failed to allocate PID\n");
        kfree(stack);
        proc_table_free(proc);
        return NULL;
    }
    
    proc->pid               = pid;
    proc->state             = PROC_EMBRYO;
    proc->priority          = DEFAULT_PRIORITY;
    proc->kernel_stack      = (uint64_t)stack;
    proc->kernel_stack_size = stack_size;
    
    // Set process name
    if (name) {
        for (int i = 0; i < 31 && name[i]; i++) {
            proc->name[i] = name[i];
        }
        proc->name[31] = '\0';
    } else {
        const char* default_name = "kernel_thread";
        for (int i = 0; i < 31 && default_name[i]; i++) {
            proc->name[i] = default_name[i];
        }
        proc->name[31] = '\0';
    }

    // Set up stack for x86-64 ABI
    uint64_t *top = (uint64_t *)((uint64_t)stack + stack_size);
    top = (uint64_t *)((uint64_t)top & ~(uint64_t)0xF); // 16-byte align
    *(--top) = (uint64_t)proc_exit;  // Return address when entry() returns

    proc->context.rsp    = (uint64_t)top;
    proc->context.rip    = (uint64_t)entry;
    proc->context.rflags = DEFAULT_RFLAGS;
    
    __asm__ volatile("mov %%cr3, %0" : "=r"(proc->context.cr3));
    
    proc->context.cs = 0x08;
    proc->context.ds = 0x10;
    proc->context.es = 0x10;
    proc->context.fs = 0x10;
    proc->context.gs = 0x10;
    proc->context.ss = 0x10;

    // Statistics
    proc->creation_time = timer_get_ticks();
    proc->cpu_time_used = 0;

    proc->time_slice = SCHED_QUANTUM;
    proc->ticks_left = SCHED_QUANTUM;

    /* Link into the shared process/tree lists with preemption held off. */
    irqflags_t f = local_irq_save();
    if (current_process) {
        proc->parent_pid = current_process->pid;
        proc_add_child(current_process, proc);
    } else {
        proc->parent_pid = 0;  // No parent (should only happen for idle/init)
    }
    all_list_add(proc);
    local_irq_restore(f);

    KLOG_D("PROC", "created PID %u (%s) stack=%p parent=%u\n",
           proc->pid, proc->name, (void*)proc->kernel_stack, proc->parent_pid);

    return proc;
}
