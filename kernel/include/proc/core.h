#ifndef MAKHOS_PROC_CORE_H
#define MAKHOS_PROC_CORE_H

/**
 * =============================================================================
 * proc/core.h - Core Process Management
 * =============================================================================
 * Phase 9: Basic process management - context switching and scheduling
 * 
 * Provides:
 *   - Context structure (saved registers)
 *   - Process state enumeration
 *   - Basic process operations: yield, exit, init
 * =============================================================================
 */

#include <types.h>

// =============================================================================
// PROCESS STATE ENUMERATION
// =============================================================================

typedef enum {
    PROC_EMBRYO,     // Being created
    PROC_READY,      // Ready to run
    PROC_RUNNING,    // Currently executing
    PROC_BLOCKED,    // Waiting for I/O
    PROC_ZOMBIE      // Terminated, waiting for parent
} proc_state_t;

// =============================================================================
// PROCESS CONTEXT (Saved Registers)
// =============================================================================

typedef struct {
    // General Purpose Registers (128 bytes)
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    
    // Stack and Instruction Pointers (24 bytes)
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    
    // Memory Management (8 bytes)
    uint64_t cr3;
    
    // Segment Registers (48 bytes)
    uint64_t cs, ds, es, fs, gs, ss;
} __attribute__((packed)) context_t;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================

typedef struct process process_t;

// =============================================================================
// CORE PROCESS API
// =============================================================================

/**
 * proc_init - Initialize process manager
 */
void proc_init(void);

/**
 * proc_yield - Yield CPU to next process (scheduler)
 */
void proc_yield(void);

/**
 * proc_current - Get currently running process
 */
process_t* proc_current(void);

/**
 * proc_get_pid - Get current process PID
 */
uint32_t proc_get_pid(void);

#endif // MAKHOS_PROC_CORE_H
