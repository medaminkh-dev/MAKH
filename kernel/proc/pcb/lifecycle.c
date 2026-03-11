#include <proc/core.h>
#include <proc/pcb.h>
#include <kernel.h>
#include <vga.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * kernel/proc/pcb/lifecycle.c - Process Lifecycle Management
 * =============================================================================
 * FIXES APPLIED:
 *
 *  BUG#6:  proc_exit() was calling proc_free_pid() while still ZOMBIE.
 *          PID freed only in proc_reap() now.
 *
 *  BUG#10: proc_create() no longer calls proc_add_to_ready() to avoid
 *          next/prev pointer corruption between all_processes and ready_queue.
 *          Callers must explicitly call proc_add_to_ready() if needed.
 *
 *  BUG-PID-LEAK: proc_init() was calling proc_create() (which calls
 *          proc_alloc_pid()) then overriding the PID manually → unused
 *          PID entries in bitmap. Fixed by using a new helper
 *          proc_create_with_pid() that reserves the exact PID.
 *
 *  TEST4-FIX: proc_create() returns PROC_EMBRYO. Tests that check
 *          state == PROC_READY after proc_create() need proc_add_to_ready()
 *          to be called. proc_create() now sets state to PROC_EMBRYO correctly;
 *          callers call proc_add_to_ready() which sets PROC_READY.
 * =============================================================================
 */

// =============================================================================
// EXTERNAL DECLARATIONS
// =============================================================================

extern void scheduler_init(void);
extern void scheduler_set_current(process_t* proc);
extern void scheduler_add_to_all_processes(process_t* proc);
extern void scheduler_remove_from_all_processes(process_t* proc);
extern process_t* scheduler_get_all_processes_head(void);
extern void proc_add_to_ready(process_t* proc);
extern process_t* proc_current(void);

extern void proc_add_child(process_t* parent, process_t* child);
extern void proc_remove_child(process_t* child);
extern void proc_reparent_orphans(process_t* dead_parent);

extern process_t* proc_table_alloc(void);
extern void proc_table_free(process_t* proc);
extern int32_t proc_alloc_pid(void);
extern void proc_free_pid(int32_t pid);
extern void proc_table_init(void);
extern void proc_reserve_pid(int32_t pid);  // NEW in table.c

// =============================================================================
// INTERNAL HELPER: create process with a specific reserved PID
// Used only by proc_init() to create idle (PID 0) and init (PID 1)
// =============================================================================

static process_t* proc_create_with_pid(int32_t pid, uint64_t stack_size) {
    process_t* proc = proc_table_alloc();
    if (!proc) return NULL;

    memset(proc, 0, sizeof(process_t));

    // Reserve this exact PID in the bitmap
    proc_reserve_pid(pid);
    proc->pid = (uint32_t)pid;

    proc->state = PROC_READY;  // directly READY for idle/init
    proc->priority = 128;
    proc->creation_time = timer_get_ticks();
    proc->cpu_time_used = 0;
    proc->parent_pid = 0;
    proc->child_count = 0;
    proc->children_head = NULL;
    proc->children_tail = NULL;
    proc->exit_code = 0;

    proc->kernel_stack_size = stack_size;
    proc->kernel_stack = (uint64_t)kmalloc(stack_size);
    if (!proc->kernel_stack) {
        proc_free_pid(pid);
        proc_table_free(proc);
        return NULL;
    }
    memset((void*)proc->kernel_stack, 0, stack_size);
    memset(&proc->context, 0, sizeof(context_t));

    proc->context.rflags = 0x202;
    proc->context.cs = 0x08;
    proc->context.ds = 0x10;
    proc->context.es = 0x10;
    proc->context.fs = 0x10;
    proc->context.gs = 0x10;
    proc->context.ss = 0x10;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    proc->context.cr3 = cr3;

    scheduler_add_to_all_processes(proc);
    return proc;
}

// =============================================================================
// PROCESS INITIALIZATION
// =============================================================================

void proc_init(void) {
    terminal_writestring("[PROC] Initializing process manager...\n");

    proc_table_init();
    scheduler_init();

    // FIX PID-LEAK: Use proc_create_with_pid() to reserve exact PIDs
    // without wasting bitmap slots. PID 0 is already reserved in pid_bitmap_init().

    // Idle process (PID 0)
    process_t* idle = proc_create_with_pid(0, 4096);
    if (idle) {
        idle->priority = 0;  // lowest priority
        const char* n = "idle";
        for (int i = 0; i < 4; i++) idle->name[i] = n[i];
        idle->name[4] = '\0';
        terminal_writestring("[PROC] Idle process created (PID 0)\n");
    }

    // Init process (PID 1)
    process_t* init = proc_create_with_pid(1, 16384);
    if (init) {
        const char* n = "init";
        for (int i = 0; i < 4; i++) init->name[i] = n[i];
        init->name[4] = '\0';
        terminal_writestring("[PROC] Init process created (PID 1)\n");
    }

    // Set current process to idle
    scheduler_set_current(idle);

    terminal_writestring("[PROC] Process manager initialized\n");
}

// =============================================================================
// PROC CREATE
// =============================================================================

/**
 * proc_create - Create a new kernel process
 *
 * FIX BUG#10: Does NOT call proc_add_to_ready(). Caller is responsible.
 *             This prevents next/prev pointer aliasing between two lists.
 *
 * FIX TEST4:  Returns process in PROC_EMBRYO state. After caller does any
 *             extra setup, caller calls proc_add_to_ready() → state=PROC_READY.
 *             Tests should call proc_add_to_ready() after proc_create() if
 *             they need PROC_READY state.
 *
 * @entry: Entry function (NULL = no auto setup)
 * @stack_size: Kernel stack size
 * @return: New process or NULL
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size) {
    terminal_writestring("[PROC] Creating new process...\n");

    process_t* proc = proc_table_alloc();
    if (!proc) {
        terminal_writestring("[PROC] Failed to allocate process table slot\n");
        return NULL;
    }

    memset(proc, 0, sizeof(process_t));

    proc->pid = proc_alloc_pid();
    if (proc->pid < 0) {
        terminal_writestring("[PROC] Failed to allocate PID\n");
        proc_table_free(proc);
        return NULL;
    }

    proc->state = PROC_EMBRYO;
    proc->priority = 128;
    proc->creation_time = timer_get_ticks();
    proc->cpu_time_used = 0;
    proc->parent_pid = 0;
    proc->child_count = 0;
    proc->children_head = NULL;
    proc->children_tail = NULL;
    proc->exit_code = 0;

    const char* default_name = "kernel_thread";
    for (int i = 0; i < 31 && default_name[i]; i++) {
        proc->name[i] = default_name[i];
    }
    proc->name[31] = '\0';

    proc->kernel_stack_size = stack_size;
    proc->kernel_stack = (uint64_t)kmalloc(stack_size);
    if (!proc->kernel_stack) {
        terminal_writestring("[PROC] Failed to allocate stack\n");
        proc_free_pid(proc->pid);
        proc_table_free(proc);
        return NULL;
    }

    memset((void*)proc->kernel_stack, 0, stack_size);
    memset(&proc->context, 0, sizeof(context_t));

    if (entry) {
        uint64_t* stack_top = (uint64_t*)(proc->kernel_stack + stack_size);
        *(--stack_top) = (uint64_t)entry;
        *(--stack_top) = (uint64_t)proc_exit;
        proc->context.rsp = (uint64_t)stack_top;
        proc->context.rip = (uint64_t)entry;
    }

    proc->context.rflags = 0x202;
    proc->context.cs = 0x08;
    proc->context.ds = 0x10;
    proc->context.es = 0x10;
    proc->context.fs = 0x10;
    proc->context.gs = 0x10;
    proc->context.ss = 0x10;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    proc->context.cr3 = cr3;

    // Add to all_processes list (uses next/prev)
    scheduler_add_to_all_processes(proc);

    // FIX BUG#10: DO NOT call proc_add_to_ready() here.

    // Add to parent's children list
    process_t* parent = proc_current();
    if (parent) {
        proc->parent_pid = parent->pid;
        proc_add_child(parent, proc);
    }

    terminal_writestring("[PROC] Process created with PID ");
    char buf[16];
    uint64_to_string(proc->pid, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");

    return proc;
}

// =============================================================================
// PROC EXIT
// =============================================================================

/**
 * proc_exit - Exit current process
 *
 * FIX BUG#6: Does NOT free PID. PID freed only when parent calls proc_reap().
 */
void proc_exit(int code) {
    process_t* curr = proc_current();
    if (!curr) return;

    char buf[32];
    terminal_writestring("[PROC] Process PID ");
    uint64_to_string(curr->pid, buf);
    terminal_writestring(buf);
    terminal_writestring(" exiting with code ");
    uint64_to_string((uint64_t)code, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");

    curr->exit_code = code;
    curr->state = PROC_ZOMBIE;

    proc_reparent_orphans(curr);

    if (curr->parent_pid != 0 && curr->parent_pid != curr->pid) {
        proc_remove_child(curr);
    }

    // FIX BUG#6: REMOVED proc_free_pid() — parent does it via proc_reap()

    proc_yield();

    for(;;) __asm__ volatile("hlt");
}

// =============================================================================
// PROC REAP (NEW - FIX BUG#6)
// =============================================================================

/**
 * proc_reap - Reap a zombie process after parent reads exit code
 */
int proc_reap(process_t* zombie) {
    if (!zombie || zombie->state != PROC_ZOMBIE) return -1;

    int exit_code = zombie->exit_code;

    proc_free_pid((int32_t)zombie->pid);
    scheduler_remove_from_all_processes(zombie);

    if (zombie->kernel_stack) {
        kfree((void*)zombie->kernel_stack);
        zombie->kernel_stack = 0;
    }

    proc_table_free(zombie);
    return exit_code;
}
