#include <proc.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <vga.h>
#include <lib/string.h>

/**
 * =============================================================================
 * proc.c - Process Management Implementation for MakhOS
 * =============================================================================
 * This file implements the process management subsystem including:
 *   - Process creation and destruction
 *   - Round-robin scheduling
 *   - Context switching coordination
 *   - Process queues (ready queue, all processes list)
 *
 * Phase 9: Process Management - NEW FILE
 * =============================================================================
 */

// =============================================================================
// GLOBAL VARIABLES
// =============================================================================

/**
 * in_interrupt_context - Flag for interrupt handling
 * 
 * PHASE 9 CHANGE (NEW):
 * Set to 1 by timer_handler before calling proc_yield()
 * Checked by proc_yield() to skip context switch during interrupts
 * This prevents nested context switches which would crash the kernel
 */
volatile int in_interrupt_context = 0;

/**
 * current_process - Currently running process
 * 
 * PHASE 9 CHANGE (NEW):
 * Points to the PCB of the currently executing process
 */
static process_t* current_process = NULL;

/**
 * ready_queue - Queue of processes ready to run
 * 
 * PHASE 9 CHANGE (NEW):
 * Round-robin scheduling uses this queue
 * Processes are dequeued from head, enqueued to tail
 */
static process_list_t ready_queue;

/**
 * all_processes - List of all processes in system
 * 
 * PHASE 9 CHANGE (NEW):
 * Used to track all processes for cleanup and management
 */
static process_list_t all_processes;

/**
 * next_pid - Next available process ID
 * 
 * PHASE 9 CHANGE (NEW):
 * Atomic counter for assigning unique PIDs
 */
static uint32_t next_pid = 1;

// Forward declarations
static void proc_idle(void);

// Helper function to print hex number
static void print_hex(uint64_t val) {
    char buf[32];
    uint64_to_hex(val, buf);
    terminal_writestring(buf);
}

/**
 * proc_init - Initialize the process manager
 * 
 * PHASE 9 CHANGE (NEW):
 * Sets up:
 *   - Empty ready queue
 *   - Empty all_processes list  
 *   - Creates idle process (PID 0)
 *   - Creates init process (PID 1)
 *   - Sets current_process to idle
 */
void proc_init(void) {
    terminal_writestring("[PROC] Initializing process manager...\n");
    
    // Initialize process lists
    ready_queue.head = NULL;
    ready_queue.tail = NULL;
    ready_queue.count = 0;
    
    all_processes.head = NULL;
    all_processes.tail = NULL;
    all_processes.count = 0;
    
    // Create idle process (PID 0) - runs when no other processes
    process_t* idle = proc_create(proc_idle, 4096);
    if (idle) {
        idle->pid = 0;
        idle->state = PROC_READY;
        terminal_writestring("[PROC] Idle process created (PID 0)\n");
    }
    
    // Create init process (PID 1) - first user process
    // FIX: Use proc_idle as entry to prevent crash (NULL would cause jump to 0)
    process_t* init = proc_create(proc_idle, 16384);
    if (init) {
        init->pid = 1;
        init->state = PROC_READY;
        terminal_writestring("[PROC] Init process created (PID 1)\n");
    }
    
    // Set current process to idle initially
    current_process = idle;
    
    terminal_writestring("[PROC] Ready queue: ");
    print_hex(ready_queue.count);
    terminal_writestring(" processes\n");
}

/**
 * proc_create - Create a new process
 * 
 * PHASE 9 CHANGE (NEW):
 * Allocates and initializes a new process:
 *   1. Allocate PCB (process control block)
 *   2. Allocate kernel stack
 *   3. Set up initial context (registers, stack)
 *   4. Add to all_processes list
 * 
 * @entry: Function pointer to start executing
 * @stack_size: Size of kernel stack
 * @return: process_t* or NULL on failure
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size) {
    terminal_writestring("[PROC] Creating new process...\n");
    
    // Allocate PCB
    process_t* proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        terminal_writestring("[PROC] Failed to allocate PCB\n");
        return NULL;
    }
    
    // Clear PCB
    memset(proc, 0, sizeof(process_t));
    
    // Set basic info
    proc->pid = next_pid++;
    proc->state = PROC_EMBRYO;
    
    // Allocate kernel stack
    proc->kernel_stack_size = stack_size;
    proc->kernel_stack = (uint64_t)kmalloc(stack_size);
    if (!proc->kernel_stack) {
        terminal_writestring("[PROC] Failed to allocate kernel stack\n");
        kfree(proc);
        return NULL;
    }
    
    // Clear stack (optional)
    memset((void*)proc->kernel_stack, 0, stack_size);
    
    // Initialize context for first run
    memset(&proc->context, 0, sizeof(context_t));
    
    // Set up initial stack for context switch
    uint64_t* stack_top = (uint64_t*)(proc->kernel_stack + stack_size);
    
    // Push return address (function to run)
    *(--stack_top) = (uint64_t)entry;
    
    // Push fake return address (for when function returns)
    *(--stack_top) = (uint64_t)proc_exit;
    
    // Set stack pointer in context
    proc->context.rsp = (uint64_t)stack_top;
    
    // Set instruction pointer (will be popped by context switch)
    proc->context.rip = (uint64_t)entry;
    
    // Set default flags (interrupts enabled)
    proc->context.rflags = 0x202;  // IF=1, reserved bit 1
    
    // Set CR3 (kernel page tables for now)
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    proc->context.cr3 = cr3;
    
    // Set segment registers
    proc->context.cs = 0x08;   // Kernel code
    proc->context.ds = 0x10;   // Kernel data
    proc->context.es = 0x10;
    proc->context.fs = 0x10;
    proc->context.gs = 0x10;
    proc->context.ss = 0x10;
    
    // Add to all processes list
    proc->next = NULL;
    if (all_processes.tail) {
        all_processes.tail->next = proc;
        proc->prev = all_processes.tail;
        all_processes.tail = proc;
    } else {
        all_processes.head = proc;
        all_processes.tail = proc;
        proc->prev = NULL;
    }
    all_processes.count++;
    
    terminal_writestring("[PROC] Process created: PID ");
    print_hex(proc->pid);
    terminal_writestring(", stack at 0x");
    print_hex(proc->kernel_stack);
    terminal_writestring("\n");
    
    return proc;
}

/**
 * proc_exit - Exit current process
 * 
 * PHASE 9 CHANGE (NEW):
 * Handles process termination:
 *   1. Print exit message
 *   2. Set state to ZOMBIE
 *   3. Call proc_yield() to switch to next process
 * 
 * @code: Exit code for parent to collect
 */
void proc_exit(int code) {
    terminal_writestring("[PROC] Process ");
    print_hex(current_process->pid);
    terminal_writestring(" exiting with code ");
    print_hex(code);
    terminal_writestring("\n");
    
    current_process->state = PROC_ZOMBIE;
    
    // TODO: Clean up resources
    
    // Switch to next process
    proc_yield();
    
    // Never reaches here
    for(;;);
}

/**
 * proc_yield - Yield CPU to next process (scheduler)
 * 
 * PHASE 9 CHANGE (NEW + FIX):
 * Implements round-robin scheduling:
 *   1. Get next process from ready queue head
 *   2. Put current process at queue tail (if still ready)
 *   3. Skip context switch if in_interrupt_context (FIX for nested interrupts)
 *   4. Call context_switch() to switch to new process
 * 
 * FIX: Added check for in_interrupt_context to prevent crashes
 * when timer interrupt fires during kernel code execution
 */
void proc_yield(void) {
    process_t* next = NULL;
    
    // Only print debug info if NOT in interrupt context
    if (!in_interrupt_context) {
        terminal_writestring("[PROC] proc_yield called, ready_queue.count=");
        print_hex(ready_queue.count);
        terminal_writestring(", current_process=");
        print_hex((uint64_t)current_process);
        if (current_process) {
            terminal_writestring(", pid=");
            print_hex(current_process->pid);
        }
        terminal_writestring("\n");
    }
    
    // Simple round-robin: get next process from ready queue
    if (ready_queue.count > 0) {
        // Rotate queue
        next = ready_queue.head;
        if (next) {
            ready_queue.head = next->next;
            if (ready_queue.head) {
                ready_queue.head->prev = NULL;
            } else {
                ready_queue.tail = NULL;
            }
            ready_queue.count--;
            
            // Put current process back in queue if still ready
            if (current_process && current_process->state == PROC_RUNNING) {
                current_process->state = PROC_READY;
                // Add to end of queue
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
    
    // If no next process, use idle
    if (!next) {
        // Find idle process (PID 0)
        process_t* p = all_processes.head;
        while (p) {
            if (p->pid == 0) {
                next = p;
                break;
            }
            p = p->next;
        }
    }
    
    /**
     * PHASE 9 FIX: Skip context switch if in interrupt context
     * 
     * This prevents crashes when timer interrupt fires while kernel
     * code is executing (e.g., during terminal_writestring).
     * Without this check, trying to context switch during interrupt
     * handling would cause nested exceptions.
     */
    if (in_interrupt_context) {
        return;
    }
    
    if (next && next != current_process) {
        // Switch to next process
        process_t* old = current_process;
        current_process = next;
        current_process->state = PROC_RUNNING;
        
        // Save old context and load new context
        if (old) {
            context_switch(&old->context, &current_process->context);
        } else {
            // First switch
            context_switch(NULL, &current_process->context);
        }
    }
}

process_t* proc_current(void) {
    return current_process;
}

/**
 * proc_add_to_ready - Add process to ready queue
 * 
 * PHASE 9 CHANGE (NEW):
 * Allows external code to add processes to the scheduler.
 * Used by test_context_switch to add test processes.
 * 
 * @proc: Process to add to ready queue
 */
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

uint32_t proc_get_pid(void) {
    return current_process ? current_process->pid : 0;
}

// Idle process - runs when nothing else to do
/**
 * proc_idle - Idle process (PID 0)
 * 
 * PHASE 9 CHANGE (NEW):
 * Called when no other processes are ready to run.
 * Uses hlt instruction to save power until next interrupt.
 */
static void proc_idle(void) {
    terminal_writestring("[PROC] Idle process started (PID 0)\n");
    
    while(1) {
        __asm__ volatile("hlt");  // Save power
        proc_yield();
    }
}
