#ifndef MAKHOS_PROC_H
#define MAKHOS_PROC_H

/**
 * =============================================================================
 * proc.h - Process Management Header for MakhOS
 * =============================================================================
 * This header defines the structures and functions for process management
 * and preemptive multitasking in MakhOS.
 *
 * Phase 9: Process Management - NEW FILE
 * 
 * Key Components:
 *   - Process Control Block (PCB): process_t structure
 *   - Process Context: context_t structure (saved registers)
 *   - Process States: Enum for process lifecycle
 *   - API Functions: create, destroy, schedule processes
 * =============================================================================
 */

#include <types.h>

// =============================================================================
// PROCESS LIMITS AND CONSTANTS
// =============================================================================
#define MAX_PROCESSES 256
#define PID_MAX 32768

// =============================================================================
// PROCESS STATES
// =============================================================================
/**
 * Process state enumeration
 * Represents the lifecycle states of a process in the OS
 */
typedef enum proc_state {
    PROC_EMBRYO,     // Being created (initial state)
    PROC_READY,      // Ready to run (in ready queue)
    PROC_RUNNING,    // Currently running on CPU
    PROC_BLOCKED,    // Waiting for I/O or event
    PROC_ZOMBIE      // Terminated, waiting for parent to collect
} proc_state_t;

// =============================================================================
// PROCESS CONTEXT (Saved Registers)
// =============================================================================
/**
 * Process context structure - saved during context switch
 * Total size: 200 bytes (0xC8)
 * This mirrors the layout in context_switch.asm
 */
typedef struct context {
    // -------------------------------------------------------------------------
    // General Purpose Registers (16 registers × 8 bytes = 128 bytes)
    // -------------------------------------------------------------------------
    uint64_t rax, rbx, rcx, rdx;  // Accumulator, base, counter, data
    uint64_t rsi, rdi, rbp;        // Source, destination, base pointer
    uint64_t r8, r9, r10, r11;    // Additional GPRs
    uint64_t r12, r13, r14, r15;  // More GPRs
    
    // -------------------------------------------------------------------------
    // Stack and Instruction Pointers (24 bytes)
    // -------------------------------------------------------------------------
    uint64_t rsp;  // Stack pointer
    uint64_t rip;  // Instruction pointer (program counter)
    uint64_t rflags; // Flags register (IF, TF, etc.)
    
    // -------------------------------------------------------------------------
    // Memory Management (8 bytes)
    // -------------------------------------------------------------------------
    uint64_t cr3;  // Page table base address
    
    // -------------------------------------------------------------------------
    // Segment Registers (56 bytes = 7 × 8 bytes)
    // -------------------------------------------------------------------------
    uint64_t cs, ds, es, fs, gs, ss;  // Code, data, extra, fs, gs, stack segments
} __attribute__((packed)) context_t;

// =============================================================================
// PROCESS CONTROL BLOCK (PCB)
// =============================================================================
/**
 * Process Control Block - represents a single process
 * Contains all information about a process in the OS
 */
typedef struct process {
    uint32_t pid;               // Process ID (unique identifier)
    proc_state_t state;         // Current state (EMBRYO/READY/RUNNING/BLOCKED/ZOMBIE)
    
    context_t context;          // Saved CPU registers (context for switching)
    
    uint64_t kernel_stack;      // Kernel mode stack base address
    uint64_t kernel_stack_size; // Kernel stack size in bytes
    
    uint64_t user_stack;        // User mode stack base address
    uint64_t user_stack_size;   // User stack size in bytes
    
    struct process* next;       // Next process in linked list
    struct process* prev;       // Previous process in linked list
    
    uint64_t time_slice;        // Remaining time slice (in timer ticks)
    uint64_t total_ticks;       // Total CPU time used by this process
    
    /* NEW FIELDS FOR PHASE 11 */
    uint32_t parent_pid;                 // Parent PID
    uint32_t child_count;                // Number of children
    struct process* children_head;       // List of children
    struct process* children_tail;       // List of children
    
    uint64_t creation_time;              // In timer ticks
    uint64_t cpu_time_used;              // Total CPU time used
    uint8_t priority;                    // 0-255 (higher = more priority)
    
    char name[32];                       // Process name
    
    int exit_code;                       // Exit code
} __attribute__((packed)) process_t;

// =============================================================================
// PROCESS LIST
// =============================================================================
/**
 * Process list structure for managing multiple processes
 * Used for ready queue and all processes list
 */
typedef struct {
    process_t* head;     // First process in list
    process_t* tail;     // Last process in list
    uint32_t count;      // Number of processes in list
} process_list_t;

// =============================================================================
// PROCESS MANAGEMENT API
// =============================================================================

/**
 * proc_init - Initialize the process manager
 * 
 * Creates the idle process (PID 0) and init process (PID 1)
 * Initializes the ready queue and all processes list
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Core initialization for process management
 */
void proc_init(void);

/**
 * proc_create - Create a new process
 * 
 * @entry: Function pointer where new process will start execution
 * @stack_size: Size of kernel stack to allocate
 * @return: Pointer to new process PCB, or NULL on failure
 * 
 * Allocates PCB and kernel stack, sets up initial context
 * Adds process to all_processes list
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Creates new processes in the system
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size);

/**
 * proc_exit - Exit current process
 * 
 * @code: Exit code to return to parent
 * 
 * Changes process state to ZOMBIE and yields CPU
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Handles process termination
 */
void proc_exit(int code);

/**
 * proc_yield - Yield CPU to next process (scheduler)
 * 
 * Implements round-robin scheduling:
 * 1. Get next process from ready queue
 * 2. Put current process back in queue (if still ready)
 * 3. Switch to next process via context_switch()
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Core scheduler implementation
 *   - FIX: Added check for in_interrupt_context to prevent
 *          nested context switches during interrupt handling
 */
void proc_yield(void);

/**
 * proc_add_to_ready - Add process to ready queue
 * 
 * @proc: Process to add to ready queue
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Allows external code to add processes to scheduler
 */
void proc_add_to_ready(process_t* proc);

/**
 * proc_current - Get current running process
 * 
 * @return: Pointer to current process PCB
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Query current process
 */
process_t* proc_current(void);

/**
 * proc_get_pid - Get current process PID
 * 
 * @return: PID of current process, or 0 if no process
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Syscall support for getpid()
 */
uint32_t proc_get_pid(void);

// =============================================================================
// GLOBAL FLAGS
// =============================================================================

/**
 * in_interrupt_context - Flag indicating interrupt handling
 * 
 * Set by timer_handler before calling proc_yield()
 * Checked by proc_yield() to skip context switch during interrupts
 * 
 * Changes in Phase 9:
 *   - NEW VARIABLE: Prevents nested context switches
 */
extern volatile int in_interrupt_context;

// =============================================================================
// CONTEXT SWITCH (Assembly)
// =============================================================================

/**
 * context_switch - Assembly function to switch process context
 * 
 * @old: Pointer to save current process context (can be NULL)
 * @new: Pointer to load new process context
 * 
 * Implemented in context_switch.asm
 * Saves all registers to old context, loads all from new context
 * 
 * Changes in Phase 9:
 *   - NEW FUNCTION: Low-level context switching
 */
void context_switch(context_t* old, context_t* new);

// =============================================================================
// PROCESS TABLE FUNCTIONS (Phase 11)
// =============================================================================

// Process table access functions
process_t* proc_table_alloc(void);
void proc_table_free(process_t* proc);
process_t* proc_find(int32_t pid);
int32_t proc_alloc_pid(void);
void proc_free_pid(int32_t pid);

// =============================================================================
// FORK SUPPORT (Phase 12)
// =============================================================================

/**
 * proc_fork - Fork current process
 * 
 * Creates a child process that is a copy of the parent.
 * 
 * @return: Child's PID to parent, 0 to child, -1 on failure
 */
int proc_fork(void);

#endif
