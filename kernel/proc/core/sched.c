#include <proc/core.h>
#include <proc/pcb.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <arch/tss.h>

/**
 * =============================================================================
 * kernel/proc/core/sched.c - Scheduling Implementation
 * =============================================================================
 * FIXES APPLIED:
 *   BUG#10: all_processes list and ready_queue BOTH use process_t->next/prev.
 *           When a process is in all_processes, adding it to ready_queue
 *           corrupts all_processes linked list (same pointers).
 *           FIX: Use SEPARATE linked list pointers for ready_queue:
 *                ready_queue now uses ready_next/ready_prev (new fields).
 *
 *   NOTE: The process_t struct needs ready_next/ready_prev fields added.
 *         See pcb.h fix. For now we use a separate approach:
 *         all_processes uses next/prev, ready_queue uses a parallel array.
 *
 *   ALTERNATIVE FIX (simpler): use an array-based ready queue instead of
 *   linked list, avoiding pointer aliasing completely.
 *
 *   BUG#2 related: On context switch, call tss_set_kernel_stack() to update
 *   the kernel stack pointer for the next process.
 * =============================================================================
 */

// =============================================================================
// GLOBAL STATE
// =============================================================================

volatile int in_interrupt_context = 0;
static process_t* current_process = NULL;

// FIX BUG#10: Use ARRAY-based ready queue instead of linked list
// This avoids the next/prev pointer aliasing between all_processes and ready_queue
#define READY_QUEUE_SIZE 256
static process_t* ready_queue_array[READY_QUEUE_SIZE];
static int ready_queue_head = 0;  // index of first element
static int ready_queue_tail = 0;  // index of next empty slot
static int ready_queue_count = 0;

// all_processes still uses linked list (next/prev)
static process_list_t all_processes = {0};

extern void context_switch(context_t* old, context_t* new);
static void proc_idle(void);

// =============================================================================
// READY QUEUE (ARRAY-BASED - FIX BUG#10)
// =============================================================================

static int ready_queue_enqueue(process_t* proc) {
    if (ready_queue_count >= READY_QUEUE_SIZE) return -1;
    ready_queue_array[ready_queue_tail] = proc;
    ready_queue_tail = (ready_queue_tail + 1) % READY_QUEUE_SIZE;
    ready_queue_count++;
    return 0;
}

static process_t* ready_queue_dequeue(void) {
    if (ready_queue_count == 0) return NULL;
    process_t* proc = ready_queue_array[ready_queue_head];
    ready_queue_head = (ready_queue_head + 1) % READY_QUEUE_SIZE;
    ready_queue_count--;
    return proc;
}

static void ready_queue_remove(process_t* proc) {
    // Linear scan to remove specific process (e.g., when it exits)
    for (int i = 0; i < ready_queue_count; i++) {
        int idx = (ready_queue_head + i) % READY_QUEUE_SIZE;
        if (ready_queue_array[idx] == proc) {
            // Shift remaining elements
            for (int j = i; j < ready_queue_count - 1; j++) {
                int cur = (ready_queue_head + j) % READY_QUEUE_SIZE;
                int nxt = (ready_queue_head + j + 1) % READY_QUEUE_SIZE;
                ready_queue_array[cur] = ready_queue_array[nxt];
            }
            ready_queue_tail = (ready_queue_tail - 1 + READY_QUEUE_SIZE) % READY_QUEUE_SIZE;
            ready_queue_count--;
            return;
        }
    }
}

// =============================================================================
// PUBLIC SCHEDULER API
// =============================================================================

/**
 * proc_add_to_ready - Add process to ready queue (FIX BUG#10: uses array)
 */
void proc_add_to_ready(process_t* proc) {
    if (!proc) return;
    proc->state = PROC_READY;
    ready_queue_enqueue(proc);
}

/**
 * proc_yield - Yield CPU to next process (round-robin)
 *
 * FIX: After switching processes, update TSS kernel stack pointer.
 * FIX BUG#10: Uses array-based ready queue (no pointer aliasing).
 */
void proc_yield(void) {
    process_t* next = NULL;

    // Get next ready process
    next = ready_queue_dequeue();

    // Put current process back if still running/ready
    if (current_process && current_process->state == PROC_RUNNING) {
        current_process->state = PROC_READY;
        ready_queue_enqueue(current_process);
    } else if (current_process && current_process->state == PROC_ZOMBIE) {
        // Zombie: remove from ready queue if somehow still there
        ready_queue_remove(current_process);
    }

    // If no ready process, fall back to idle (PID 0)
    if (!next) {
        process_t* p = all_processes.head;
        while (p) {
            if (p->pid == 0 && p->state != PROC_ZOMBIE) {
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

        // FIX BUG#2/3: Update kernel stack pointer for next process
        // so syscall_entry uses the correct kernel stack
        if (current_process->kernel_stack) {
            uint64_t kstack_top = current_process->kernel_stack +
                                  current_process->kernel_stack_size;
            tss_set_kernel_stack(kstack_top);
        }

        if (old) {
            context_switch(&old->context, &current_process->context);
        } else {
            context_switch(NULL, &current_process->context);
        }
    }
}

process_t* proc_current(void) {
    return current_process;
}

uint32_t proc_get_pid(void) {
    return current_process ? current_process->pid : 0;
}

// =============================================================================
// HELPER
// =============================================================================

static void proc_idle(void) {
    terminal_writestring("[PROC] Idle process started (PID 0)\n");
    while(1) {
        __asm__ volatile("hlt");
        proc_yield();
    }
}

// =============================================================================
// INTERNAL INITIALIZATION
// =============================================================================

void scheduler_init(void) {
    for (int i = 0; i < READY_QUEUE_SIZE; i++) {
        ready_queue_array[i] = NULL;
    }
    ready_queue_head = 0;
    ready_queue_tail = 0;
    ready_queue_count = 0;

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
    if (!proc) return;
    // Uses next/prev pointers for doubly-linked list
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
    if (!proc) return;
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
    proc->next = NULL;
    proc->prev = NULL;
}

uint32_t scheduler_get_ready_queue_count(void) {
    return (uint32_t)ready_queue_count;
}
