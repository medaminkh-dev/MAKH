#include <proc.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <kernel.h>
#include <vga.h>
#include <lib/string.h>
#include <drivers/timer.h>

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

// =============================================================================
// PROCESS TABLE AND PID MANAGEMENT (Phase 11.2)
// =============================================================================

/**
 * process_table - Array of pointers to all processes
 * Used for O(1) lookup by PID
 */
static process_t* process_table[MAX_PROCESSES];

/**
 * process_count - Number of active processes in the system
 */
static uint32_t process_count = 0;

/**
 * pid_bitmap - Bitmap for PID allocation
 * Each bit represents a PID (0 = free, 1 = allocated)
 * Size: PID_MAX / 8 = 4096 bytes
 */
static uint8_t pid_bitmap[PID_MAX / 8];

// Forward declarations
static void proc_idle(void);

// =============================================================================
// PID MANAGEMENT HELPER FUNCTIONS (Phase 11.2)
// =============================================================================

/**
 * pid_bitmap_init - Initialize PID bitmap
 * 
 * Clears all bits and reserves PID 0 for idle process
 */
static void pid_bitmap_init(void) {
    for (int i = 0; i < PID_MAX / 8; i++) {
        pid_bitmap[i] = 0;
    }
    // Reserve PID 0 for idle
    pid_bitmap[0] |= 1;
}

/**
 * proc_alloc_pid - Allocate a new PID
 * 
 * Uses bitmap to find first free PID (starting from 1)
 * 
 * @return: Allocated PID, or -1 if no free PIDs
 */
int32_t proc_alloc_pid(void) {
    for (int i = 1; i < PID_MAX; i++) {  // Start from 1 (PID 0 is idle)
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(pid_bitmap[byte_idx] & (1 << bit_idx))) {
            pid_bitmap[byte_idx] |= (1 << bit_idx);
            return i;
        }
    }
    return -1;  // No free PIDs
}

/**
 * proc_free_pid - Free a PID
 * 
 * Releases a PID back to the bitmap
 * 
 * @pid: PID to free
 */
void proc_free_pid(int32_t pid) {
    if (pid < 0 || pid >= PID_MAX) return;
    int byte_idx = pid / 8;
    int bit_idx = pid % 8;
    pid_bitmap[byte_idx] &= ~(1 << bit_idx);
}

/**
 * proc_find - Find process by PID
 * 
 * Searches the process table for a process with matching PID
 * 
 * @pid: PID to search for
 * @return: Pointer to process if found, NULL otherwise
 */
process_t* proc_find(int32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && (int32_t)process_table[i]->pid == pid) {
            return process_table[i];
        }
    }
    return NULL;
}

/**
 * proc_table_alloc - Allocate slot in process table
 * 
 * Finds first empty slot in process_table and allocates a new process
 * 
 * @return: Pointer to new process, or NULL if table is full
 */
process_t* proc_table_alloc(void) {
    if (process_count >= MAX_PROCESSES) return NULL;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) {
            process_table[i] = kmalloc(sizeof(process_t));
            if (process_table[i]) {
                process_count++;
                return process_table[i];
            }
        }
    }
    return NULL;
}

/**
 * proc_table_free - Free slot in process table
 * 
 * Removes process from table and frees memory
 * 
 * @proc: Process to free
 */
void proc_table_free(process_t* proc) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == proc) {
            process_table[i] = NULL;
            process_count--;
            kfree(proc);
            return;
        }
    }
}

// =============================================================================
// PROCESS TREE MANAGEMENT (Phase 11.3)
// =============================================================================

/**
 * proc_add_child - Add child to parent's children list
 * 
 * Links a child process to its parent's children list for process tree tracking
 * 
 * @parent: Parent process
 * @child: Child process to add
 */
static void proc_add_child(process_t* parent, process_t* child) {
    child->parent_pid = parent->pid;
    child->next = NULL;
    
    if (parent->children_tail) {
        parent->children_tail->next = child;
        parent->children_tail = child;
    } else {
        parent->children_head = child;
        parent->children_tail = child;
    }
    parent->child_count++;
}

/**
 * proc_remove_child - Remove child from parent's children list
 * 
 * Unlinks a child process from its parent's children list
 * 
 * @child: Child process to remove
 */
static void proc_remove_child(process_t* child) {
    process_t* parent = proc_find(child->parent_pid);
    if (!parent) return;
    
    process_t* prev = NULL;
    process_t* curr = parent->children_head;
    
    while (curr) {
        if (curr == child) {
            if (prev) {
                prev->next = curr->next;
            } else {
                parent->children_head = curr->next;
            }
            if (curr == parent->children_tail) {
                parent->children_tail = prev;
            }
            parent->child_count--;
            break;
        }
        prev = curr;
        curr = curr->next;
    }
}

/**
 * proc_reparent_orphans - Reparent orphans to init (PID 1)
 * 
 * When a parent process dies, reparent all its children to init (PID 1)
 * This ensures no process becomes an orphan
 * 
 * @dead_parent: Parent process that has terminated
 */
static void proc_reparent_orphans(process_t* dead_parent) {
    process_t* child = dead_parent->children_head;
    process_t* init = proc_find(1);
    
    while (child) {
        process_t* next = child->next;
        child->parent_pid = 1;
        child->next = NULL;
        
        if (init->children_tail) {
            init->children_tail->next = child;
            init->children_tail = child;
        } else {
            init->children_head = child;
            init->children_tail = child;
        }
        init->child_count++;
        
        child = next;
    }
    dead_parent->children_head = NULL;
    dead_parent->children_tail = NULL;
    dead_parent->child_count = 0;
}

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
    
    // Initialize PID bitmap (Phase 11.2)
    pid_bitmap_init();
    
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
 * PHASE 11.4 CHANGE (MODIFIED):
 *   - Uses proc_table_alloc() instead of direct kmalloc
 *   - Uses proc_alloc_pid() for PID allocation
 *   - Initializes new fields: parent_pid, child_count, children_head,
 *     children_tail, creation_time, cpu_time_used, priority, name
 *   - Adds child to parent's children list
 *
 * @entry: Function pointer to start executing
 * @stack_size: Size of kernel stack
 * @return: process_t* or NULL on failure
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size) {
    terminal_writestring("[PROC] Creating new process...\n");
    
    // Allocate from process table instead of direct kmalloc
    process_t* proc = proc_table_alloc();
    if (!proc) {
        terminal_writestring("[PROC] Failed to allocate process table slot\n");
        return NULL;
    }
    
    // Clear PCB
    memset(proc, 0, sizeof(process_t));
    
    // Set basic info - use new PID allocator
    proc->pid = proc_alloc_pid();  // Use new PID allocator
    if (proc->pid < 0) {
        terminal_writestring("[PROC] Failed to allocate PID\n");
        proc_table_free(proc);
        return NULL;
    }
    
    // Initialize new fields (Phase 11.4)
    proc->state = PROC_EMBRYO;
    proc->priority = 128;  // Default priority (middle)
    proc->creation_time = timer_get_ticks();  // Get current time
    proc->cpu_time_used = 0;
    proc->parent_pid = 0;  // Will be set below
    proc->child_count = 0;
    proc->children_head = NULL;
    proc->children_tail = NULL;
    proc->exit_code = 0;
    
    // Set default name
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
    
    // Add to ready queue
    proc_add_to_ready(proc);
    
    // Add to process tree (parent is current process)
    process_t* parent = proc_current();
    if (parent) {
        proc->parent_pid = parent->pid;
        proc_add_child(parent, proc);
    } else {
        // No parent (shouldn't happen), set as own parent
        proc->parent_pid = proc->pid;
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
 * PHASE 11.6 CHANGE (MODIFIED):
 * Handles process termination:
 *   1. Print exit message
 *   2. Store exit code in process structure
 *   3. Set state to ZOMBIE
 *   4. Reparent orphans to init (PID 1)
 *   5. Remove from parent's children list
 *   6. Free PID (but NOT process table slot - parent may need it)
 *   7. Call proc_yield() to switch to next process
 * 
 * @code: Exit code for parent to collect
 */
void proc_exit(int code) {
    char buf[32];
    terminal_writestring("[PROC] Process ");
    uint64_to_hex(current_process->pid, buf);
    terminal_writestring(buf);
    terminal_writestring(" exiting with code ");
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Store exit code
    current_process->exit_code = code;
    
    // Set state to zombie
    current_process->state = PROC_ZOMBIE;
    
    // Reparent orphans to init (PID 1)
    proc_reparent_orphans(current_process);
    
    // Remove from parent's children list
    if (current_process->parent_pid != 0 && current_process->parent_pid != current_process->pid) {
        proc_remove_child(current_process);
    }
    
    // Free the PID (but NOT the process table slot - parent may need to read it)
    proc_free_pid(current_process->pid);
    
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
