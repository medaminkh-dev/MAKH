#include <proc/core.h>
#include <proc/pcb.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>
#include <lib/string.h>

/**
 * =============================================================================
 * kernel/proc/core/sched.c - Scheduling Implementation
 * =============================================================================
 * Phase 9: Round-robin scheduler and process yielding
 * =============================================================================
 */

// =============================================================================
// GLOBAL STATE
// =============================================================================

volatile int in_interrupt_context = 0;
static process_t* current_process = NULL;
static process_list_t ready_queue = {0};
static process_list_t all_processes = {0};

// Forward declarations
extern void context_switch(context_t* old, context_t* new);
static void proc_idle(void);

// =============================================================================
// PUBLIC SCHEDULER API
// =============================================================================

/**
 * proc_yield - Yield CPU to next process (round-robin scheduling)
 * 
 * Implements round-robin scheduling:
 * 1. Get next process from ready queue head
 * 2. Put current process at queue tail if still ready
 * 3. Switch to next process via context_switch
 * 
 * Note: Skips context switch if in_interrupt_context to prevent
 * nested switches during interrupt handling
 */
void proc_yield(void) {
    process_t* next = NULL;
    
    // Get next process from ready queue
    if (ready_queue.count > 0) {
        next = ready_queue.head;
        if (next) {
            // Remove from head
            ready_queue.head = next->next;
            if (ready_queue.head) {
                ready_queue.head->prev = NULL;
            } else {
                ready_queue.tail = NULL;
            }
            ready_queue.count--;
            
            // Put current process back if still ready
            if (current_process && current_process->state == PROC_RUNNING) {
                current_process->state = PROC_READY;
                current_process->next = NULL;
                current_process->prev = ready_queue.tail;
                if (ready_queue.tail) {
                    ready_queue.tail->next = current_process;
                } else {
                    ready_queue.head = current_process;
                }
                ready_queue.tail = current_process;
                ready_queue.count++;
            }
        }
    }
    
    // If no next process, use idle (PID 0)
    if (!next) {
        process_t* p = all_processes.head;
        while (p) {
            if (p->pid == 0) {
                next = p;
                break;
            }
            p = p->next;
        }
    }
    
    // Skip context switch if in interrupt context
    if (in_interrupt_context) {
        return;
    }
    
    if (next && next != current_process) {
        process_t* old = current_process;
        current_process = next;
        current_process->state = PROC_RUNNING;
        
        if (old) {
            context_switch(&old->context, &current_process->context);
        }
    }
}

void proc_add_to_ready(process_t* proc) {
    if (!proc) return;
    
    proc->state = PROC_READY;
    proc->next = NULL;
    proc->prev = ready_queue.tail;
    
    if (ready_queue.tail) {
        ready_queue.tail->next = proc;
    } else {
        ready_queue.head = proc;
    }
    ready_queue.tail = proc;
    ready_queue.count++;
}

process_t* proc_current(void) {
    return current_process;
}

uint32_t proc_get_pid(void) {
    return current_process ? current_process->pid : 0;
}

// =============================================================================
// HELPER FUNCTIONS
// =============================================================================

static void proc_idle(void) {
    terminal_writestring("[PROC] Idle process started (PID 0)\n");
    while(1) {
        __asm__ volatile("hlt");
        proc_yield();
    }
}

static void helper_print_hex(uint64_t val) {
    char buf[32];
    uint64_to_hex(val, buf);
    terminal_writestring(buf);
}

// =============================================================================
// INTERNAL INITIALIZATION (called by proc_init from lifecycle.c)
// =============================================================================

void scheduler_init(void) {
    ready_queue.head = NULL;
    ready_queue.tail = NULL;
    ready_queue.count = 0;
    
    all_processes.head = NULL;
    all_processes.tail = NULL;
    all_processes.count = 0;
}

void scheduler_set_current(process_t* proc) {
    current_process = proc;
}

process_t* scheduler_get_all_processes_head(void) {
    return all_processes.head;
}

void scheduler_add_to_all_processes(process_t* proc) {
    proc->next = NULL;
    proc->prev = all_processes.tail;
    
    if (all_processes.tail) {
        all_processes.tail->next = proc;
    } else {
        all_processes.head = proc;
    }
    all_processes.tail = proc;
    all_processes.count++;
}

void scheduler_remove_from_all_processes(process_t* proc) {
    if (proc->prev) {
        proc->prev->next = proc->next;
    } else {
        all_processes.head = proc->next;
    }
    
    if (proc->next) {
        proc->next->prev = proc->prev;
    } else {
        all_processes.tail = proc->prev;
    }
    all_processes.count--;
}

uint32_t scheduler_get_ready_queue_count(void) {
    return ready_queue.count;
}
