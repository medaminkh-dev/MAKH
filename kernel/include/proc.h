/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
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
#define DEFAULT_THREAD_STACK 8192
#define STACK_CANARY_MAGIC   0x5AFEC0DE5AFEC0DEULL   /* bottom-of-stack guard */
#define PID_MAX             32768
#define MAX_PROCESSES       256

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
// WAIT QUEUE (defined before the PCB so it can be embedded by value)
// =============================================================================
//
// A FIFO of threads blocked on a condition (join, mutex, cond, sleep-on-event).
// Threads are linked via process_t.wq_next. The operations live in sched.c.

struct process;  /* forward declaration */

typedef struct wait_queue {
    struct process* head;
    struct process* tail;
} wait_queue_t;

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

    // -------- Phase 12: preemptive scheduler --------
    uint64_t time_slice;          // quantum length in ticks
    int64_t  ticks_left;          // ticks remaining in current quantum
    uint64_t wake_tick;           // tick at which a sleeping thread wakes
    void (*entry)(void*);         // thread entry point
    void* entry_arg;              // argument passed to entry
    uint8_t detached;             // 1 => auto-reaped on exit, no join needed
    uint8_t reaped;               // 1 => resources already freed
    uint8_t timed_out;            // 1 => last blocking wait ended via timeout
    int32_t preempt_count;        // >0 => this thread must not be preempted
    int     errno_val;            // per-thread errno (see errno.h)
    uint8_t stack_canary;         // 1 => kernel_stack[0..7] holds STACK_CANARY_MAGIC
    void*   fd_table;             // Phase 18: lazily-allocated file* [VFS_MAX_FDS]

    void** tls;                   // per-thread storage for pthread keys (lazy)
    void* (*pth_start)(void*);    // pthread start routine (real typed pointer)
    void*   retval;               // pthread return / exit value

    struct process* run_next;     // link within a per-priority run queue
    struct process* sleep_next;   // link within the global sleep list
    struct process* wq_next;      // link within a wait queue
    wait_queue_t    join_wq;      // threads blocked in thread_join() on us

    // Legacy Pointers (for backward compatibility)
    struct process* next;
    struct process* prev;
} process_t;   /* NOT packed: the embedded context_t must stay 8-byte aligned
                  for context_switch.asm's qword saves */

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
void proc_exit(int code) __attribute__((noreturn));
void proc_yield(void);
void proc_add_to_ready(process_t* proc);
process_t* proc_current(void);
uint32_t proc_get_pid(void);
void proc_become_current(void);

// =============================================================================
// CONTEXT SWITCH (Assembly)
// =============================================================================

void context_switch(context_t* old, context_t* new);

#endif
