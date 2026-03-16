#ifndef MAKHOS_PROC_H
#define MAKHOS_PROC_H

#include <types.h>

// =============================================================================
// CONSTANTS
// =============================================================================

#define MIN_STACK_SIZE      2048
#define MAX_STACK_SIZE      65536
#define IDLE_STACK_SIZE     16384
#define INIT_STACK_SIZE     8192
#define PID_MAX             32768

// =============================================================================
// PROCESS STATES
// =============================================================================

typedef enum proc_state {
    PROC_EMBRYO,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

// =============================================================================
// PROCESS CONTEXT (Saved Registers)
// =============================================================================

typedef struct context {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp, rip, rflags;
    uint64_t cr3;
    uint64_t cs, ds, es, fs, gs, ss;
} __attribute__((packed)) context_t;

// =============================================================================
// PROCESS CONTROL BLOCK (PCB)
// =============================================================================

typedef struct process {
    // Basic Process Info
    uint32_t pid;
    uint32_t parent_pid;
    proc_state_t state;
    uint8_t priority;
    char name[32];
    
    // Context and Memory
    context_t context;
    uint64_t kernel_stack;
    uint64_t kernel_stack_size;
    
    // All Processes List
    struct process* all_next;
    struct process* all_prev;
    
    // Ready Queue
    struct process* ready_next;
    struct process* ready_prev;
    
    // Process Tree (Phase 11)
    uint32_t child_count;
    struct process* children_head;
    struct process* children_tail;
    struct process* sibling_next;
    
    // NEW: Statistics
    uint64_t creation_time;
    uint64_t cpu_time_used;
    uint64_t exit_time;           
    
    // Exit Information
    int exit_code;
    
    // Legacy Pointers (for backward compatibility)
    struct process* next;
    struct process* prev;
} __attribute__((packed)) process_t;

// =============================================================================
// PROCESS LIST
// =============================================================================

typedef struct {
    process_t* head;
    process_t* tail;
    uint32_t count;
} process_list_t;

// =============================================================================
// PROCESS MANAGEMENT API
// =============================================================================

void proc_init(void);
process_t* proc_create(void (*entry)(void), uint64_t stack_size, const char* name);
void proc_exit(int code);
void proc_yield(void);
void proc_add_to_ready(process_t* proc);
process_t* proc_current(void);
uint32_t proc_get_pid(void);
void proc_become_current(void);


// =============================================================================
// GLOBAL FLAGS
// =============================================================================

extern volatile int in_interrupt_context;

// =============================================================================
// CONTEXT SWITCH (Assembly)
// =============================================================================

void context_switch(context_t* old, context_t* new);

#endif
