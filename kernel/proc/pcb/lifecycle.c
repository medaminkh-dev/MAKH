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
 * Phase 9+11: Process creation, exit, and initialization
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

// Forward declarations (from pcb/tree.c)
extern void proc_add_child(process_t* parent, process_t* child);
extern void proc_remove_child(process_t* child);
extern void proc_reparent_orphans(process_t* dead_parent);

// Forward declarations (from pcb/table.c)
extern process_t* proc_table_alloc(void);
extern void proc_table_free(process_t* proc);
extern int32_t proc_alloc_pid(void);
extern void proc_free_pid(int32_t pid);
extern void proc_table_init(void);

// =============================================================================
// PROCESS INITIALIZATION
// =============================================================================

void proc_init(void) {
    terminal_writestring("[PROC] Initializing process manager...\n");
    
    // Initialize process table
    proc_table_init();
    
    // Initialize scheduler
    scheduler_init();
    
    // Create idle process (PID 0)
    process_t* idle = proc_create(NULL, 4096);  // Will be set manually
    if (idle) {
        idle->pid = 0;
        idle->state = PROC_READY;
        terminal_writestring("[PROC] Idle process created (PID 0)\n");
    }
    
    // Create init process (PID 1)
    process_t* init = proc_create(NULL, 16384);
    if (init) {
        init->pid = 1;
        init->state = PROC_READY;
        terminal_writestring("[PROC] Init process created (PID 1)\n");
    }
    
    // Set current process to idle
    scheduler_set_current(idle);
    
    terminal_writestring("[PROC] Process manager initialized\n");
}

/**
 * proc_create - Create a new process
 * 
 * Allocates PCB, kernel stack, and sets up initial context.
 * Process starts in EMBRYO state and must be added to ready queue.
 * 
 * @entry: Entry function (NULL for manually-configured processes like idle/init)
 * @stack_size: Kernel stack size in bytes
 * @return: Pointer to new process, or NULL on failure
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size) {
    terminal_writestring("[PROC] Creating new process...\n");
    
    // Allocate from process table
    process_t* proc = proc_table_alloc();
    if (!proc) {
        terminal_writestring("[PROC] Failed to allocate process table slot\n");
        return NULL;
    }
    
    // Clear PCB
    memset(proc, 0, sizeof(process_t));
    
    // Allocate PID
    proc->pid = proc_alloc_pid();
    if (proc->pid < 0) {
        terminal_writestring("[PROC] Failed to allocate PID\n");
        proc_table_free(proc);
        return NULL;
    }
    
    // Initialize process fields
    proc->state = PROC_EMBRYO;
    proc->priority = 128;  // Default priority
    proc->creation_time = timer_get_ticks();
    proc->cpu_time_used = 0;
    proc->parent_pid = 0;
    proc->child_count = 0;
    proc->children_head = NULL;
    proc->children_tail = NULL;
    proc->exit_code = 0;
    
    // Set name
    const char* default_name = "kernel_thread";
    for (int i = 0; i < 31 && default_name[i]; i++) {
        proc->name[i] = default_name[i];
    }
    proc->name[31] = '\0';
    
    // Allocate kernel stack
    proc->kernel_stack_size = stack_size;
    proc->kernel_stack = (uint64_t)kmalloc(stack_size);
    if (!proc->kernel_stack) {
        terminal_writestring("[PROC] Failed to allocate stack\n");
        proc_free_pid(proc->pid);
        proc_table_free(proc);
        return NULL;
    }
    
    // Clear stack
    memset((void*)proc->kernel_stack, 0, stack_size);
    
    // Initialize context
    memset(&proc->context, 0, sizeof(context_t));
    
    if (entry) {
        // Set up stack for context switch
        uint64_t* stack_top = (uint64_t*)(proc->kernel_stack + stack_size);
        *(--stack_top) = (uint64_t)entry;
        *(--stack_top) = (uint64_t)proc_exit;
        
        proc->context.rsp = (uint64_t)stack_top;
        proc->context.rip = (uint64_t)entry;
    }
    
    // Set default flags and segments
    proc->context.rflags = 0x202;  // IF=1
    proc->context.cs = 0x08;
    proc->context.ds = 0x10;
    proc->context.es = 0x10;
    proc->context.fs = 0x10;
    proc->context.gs = 0x10;
    proc->context.ss = 0x10;
    
    // Set page table
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    proc->context.cr3 = cr3;
    
    // Add to all processes list
    scheduler_add_to_all_processes(proc);
    
    // Add to ready queue
    proc_add_to_ready(proc);
    
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

/**
 * proc_exit - Exit current process
 * 
 * Marks process as ZOMBIE and yields CPU.
 * Parent must later reap the process.
 * 
 * @code: Exit code for parent to read
 */
void proc_exit(int code) {
    process_t* curr = proc_current();
    if (!curr) return;
    
    char buf[32];
    terminal_writestring("[PROC] Process PID ");
    uint64_to_string(curr->pid, buf);
    terminal_writestring(buf);
    terminal_writestring(" exiting with code ");
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Store exit code
    curr->exit_code = code;
    curr->state = PROC_ZOMBIE;
    
    // Reparent orphans
    proc_reparent_orphans(curr);
    
    // Remove from parent's tree
    if (curr->parent_pid != 0 && curr->parent_pid != curr->pid) {
        proc_remove_child(curr);
    }
    
    // Free PID
    proc_free_pid(curr->pid);
    
    // Switch to next process
    proc_yield();
    
    for(;;);
}
