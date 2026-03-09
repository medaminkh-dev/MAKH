#include <proc/fork.h>
#include <kernel.h>
#include <vga.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * kernel/proc/fork/fork.c - Fork System Call Implementation
 * =============================================================================
 * Phase 12: Process forking - creating child copy of parent
 * =============================================================================
 */

// External declarations
extern process_t* proc_find(int32_t pid);
extern process_t* proc_table_alloc(void);
extern void proc_table_free(process_t* proc);
extern int32_t proc_alloc_pid(void);
extern void proc_free_pid(int32_t pid);
extern void proc_add_to_ready(process_t* proc);
extern void proc_add_child(process_t* parent, process_t* child);
extern process_t* proc_current(void);
extern void proc_yield(void);

int proc_fork(void) {
    process_t* parent = proc_current();
    if (!parent) return -1;
    
    terminal_writestring("[FORK] Forking process PID ");
    char buf[32];
    uint64_to_string(parent->pid, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Allocate child process
    process_t* child = proc_table_alloc();
    if (!child) return -1;
    
    memset(child, 0, sizeof(process_t));
    
    // Allocate PID
    child->pid = proc_alloc_pid();
    if (child->pid < 0) {
        proc_table_free(child);
        return -1;
    }
    
    // Copy process info
    child->state = PROC_READY;
    child->priority = parent->priority;
    child->creation_time = timer_get_ticks();
    child->cpu_time_used = 0;
    child->parent_pid = parent->pid;
    child->child_count = 0;
    child->children_head = NULL;
    child->children_tail = NULL;
    child->exit_code = 0;
    child->kernel_stack_size = parent->kernel_stack_size;
    child->user_stack_size = parent->user_stack_size;
    child->user_stack = parent->user_stack;
    
    // Copy name
    for (int i = 0; i < 32; i++) {
        child->name[i] = parent->name[i];
        if (parent->name[i] == '\0') break;
    }
    
    // Copy kernel stack
    if (copy_kernel_stack(parent, child) != 0) {
        proc_free_pid(child->pid);
        proc_table_free(child);
        return -1;
    }
    
    // Copy user memory
    if (copy_user_memory(parent, child) != 0) {
        kfree((void*)child->kernel_stack);
        proc_free_pid(child->pid);
        proc_table_free(child);
        return -1;
    }
    
    // Copy context but set child's return value to 0
    child->context = parent->context;
    child->context.rax = 0;  // Child gets 0
    
    // Add to scheduler
    proc_add_to_ready(child);
    
    // Add to parent's children
    proc_add_child(parent, child);
    
    terminal_writestring("[FORK] Created child PID ");
    uint64_to_string(child->pid, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    return child->pid;  // Parent returns child's PID
}
