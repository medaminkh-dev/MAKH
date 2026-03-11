#ifndef MAKHOS_PROC_PCB_H
#define MAKHOS_PROC_PCB_H

/**
 * =============================================================================
 * proc/pcb.h - Process Control Block and Management
 * =============================================================================
 * Phase 11: Process table and PCB management
 * 
 * Provides:
 *   - Process Control Block (PCB) structure
 *   - Process table management
 *   - Process lifecycle (create, find, exit)
 *   - Process tree (parent-child relationships)
 * =============================================================================
 */

#include <types.h>
#include <proc/core.h>

// =============================================================================
// PROCESS TABLE CONSTANTS
// =============================================================================

#define MAX_PROCESSES 256
#define PID_MAX 32768

// =============================================================================
// PROCESS FLAGS
// =============================================================================

#define PROC_FLAG_ZOMBIE      (1 << 0)
#define PROC_FLAG_SLEEPING    (1 << 1)
#define PROC_FLAG_WAITING     (1 << 2)

// =============================================================================
// PROCESS CONTROL BLOCK
// =============================================================================

typedef struct process {
    // Basic Process Info
    uint32_t pid;
    proc_state_t state;
    uint32_t parent_pid;
    
    // Context and Memory
    context_t context;
    uint64_t kernel_stack;
    uint64_t kernel_stack_size;
    uint64_t user_stack;
    uint64_t user_stack_size;
    
    // Linked List (for all_processes)
    struct process* next;
    struct process* prev;
    
    // Scheduling
    uint64_t time_slice;
    uint64_t total_ticks;
    uint8_t priority;
    
    // Process Tree (Phase 11)
    uint32_t child_count;
    struct process* children_head;
    struct process* children_tail;
    struct process* sibling_next;  // Next sibling in parent's list
    
    // Statistics (Phase 11)
    uint64_t creation_time;
    uint64_t cpu_time_used;
    char name[32];
    int exit_code;
} __attribute__((packed)) process_t;

// =============================================================================
// PROCESS LIST STRUCTURE
// =============================================================================

typedef struct {
    process_t* head;
    process_t* tail;
    uint32_t count;
} process_list_t;

// =============================================================================
// PCB MANAGEMENT API
// =============================================================================

/**
 * proc_create - Create a new process
 * @entry: Entry function
 * @stack_size: Kernel stack size
 * @return: Pointer to new process or NULL
 */
process_t* proc_create(void (*entry)(void), uint64_t stack_size);

/**
 * proc_exit - Exit current process
 * @code: Exit code
 */
void proc_exit(int code);

/**
 * proc_find - Find process by PID
 * @pid: Process ID
 * @return: Process pointer or NULL
 */
process_t* proc_find(int32_t pid);

/**
 * proc_add_to_ready - Add process to ready queue
 * @proc: Process to add
 */
void proc_add_to_ready(process_t* proc);

/**
 * proc_alloc_pid - Allocate a new PID
 * @return: Allocated PID or -1
 */
int32_t proc_alloc_pid(void);

/**
 * proc_free_pid - Free a PID
 * @pid: PID to free
 */
void proc_free_pid(int32_t pid);

#endif // MAKHOS_PROC_PCB_H

/**
 * proc_reap - Reap a zombie process (NEW - FIX BUG#6)
 * Must be called by parent after waitpid to free process resources.
 * @zombie: Zombie process to reap
 * @return: Exit code
 */
int proc_reap(process_t* zombie);

/**
 * proc_reserve_pid - Reserve a specific PID in the bitmap (NEW - FIX PID-LEAK)
 * Used by proc_init() to reserve PIDs 0 and 1 directly.
 * @pid: PID to reserve
 */
void proc_reserve_pid(int32_t pid);
