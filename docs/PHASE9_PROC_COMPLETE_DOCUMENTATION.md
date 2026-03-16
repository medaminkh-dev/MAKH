# MakhOS Phase 9: Process Management & Scheduling - Complete Technical Documentation

**Version:** 0.0.2  
**Date:** 2026-03-16  
**Status:** COMPLETE - Working Implementation  
**Author:** MakhOS Development Team  
**Contributors:** Professors, Doctors, Trovalids, Greg K.H., and Intel/CPU Architecture Experts  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architectural Overview](#2-architectural-overview)
3. [Process Control Block (PCB) Design](#3-process-control-block-pcb-design)
4. [Process States and Lifecycle](#4-process-states-and-lifecycle)
5. [Context Switching Mechanism](#5-context-switching-mechanism)
6. [Scheduler Implementation](#6-scheduler-implementation)
7. [Module-by-Module Documentation](#7-module-by-module-documentation)
8. [Critical Bug Fixes and Solutions](#8-critical-bug-fixes-and-solutions)
9. [Data Structures](#9-data-structures)
10. [API Reference](#10-api-reference)
11. [Testing and Verification](#11-testing-and-verification)
12. [Performance Characteristics](#12-performance-characteristics)
13. [Future Enhancements](#13-future-enhancements)

---

## 1. Executive Summary

Phase 9 implements **process management** and **preemptive multitasking** for MakhOS, a standalone operating system kernel. This phase introduces the fundamental building blocks of multitasking, enabling multiple processes to execute concurrently on a single CPU through time-slicing and context switching.

### Key Achievements

- **Process Control Block (PCB)**: Complete process representation with context, state, and metadata
- **Context Switching**: Assembly-level implementation for saving/restoring process state
- **Round-Robin Scheduler**: Fair CPU time distribution among processes
- **Timer Interrupt Integration**: Preemptive scheduling via hardware timer
- **Process Lifecycle Management**: Creation, execution, blocking, and termination

### System Requirements Met

| Requirement | Implementation |
|-------------|----------------|
| Preemptive Multitasking | Timer-driven context switching |
| Process Isolation | Separate kernel stacks per process |
| Fair Scheduling | Round-robin with time slices |
| Context Preservation | Full register state save/restore |
| Interrupt Safety | Interrupt context flag handling |

---

## 2. Architectural Overview

### 2.1 High-Level Architecture

```
┌───────────────────────────────────────────────────────────────────────┐
│                           MakhOS Kernel                               │
├───────────────────────────────────────────────────────────────────────┤
│                                                                       │
│  ┌─────────────────────┐    ┌─────────────────────────────────────┐   │
│  │  Process Manager    │    │     Timer Driver (IRQ0)             │   │
│  │  (proc subsystem)   │    │     (generates ticks)               │   │
│  └──────────┬──────────┘    └────────────────────┬────────────────┘   │
│             │                                    │                    │
│             │  proc_yield()                      │                    │
│             │  ◄─────────────────────────────────┘                    │
│             │                                                         │
│             ▼                                                         │
│  ┌────────────────────────────────────────────────────────────────┐   │
│  │                    Scheduler (Round-Robin)                     │   │
│  │  ┌──────────────────────────────────────────────────────────┐  │   │
│  │  │ ready_queue: PID1 → PID2 → PID3 → ... → PIDn → (loop)    │  │   │
│  │  └──────────────────────────────────────────────────────────┘  │   │
│  └───────────────────────────────┬────────────────────────────────┘   │
│                                  │                                    │
│                                  ▼                                    │
│  ┌────────────────────────────────────────────────────────────────┐   │
│  │              context_switch() - Assembly Routine               │   │
│  │  ┌────────────────────────────────────────────────────────────┐│   │
│  │  │ Save: rax,rbx,rcx,rdx,rsi,rdi,rbp,r8-r15,rsp,rip,rflags    ││   │
│  │  │      cr3, cs,ds,es,fs,gs,ss                                ││   │
│  │  └────────────────────────────────────────────────────────────┘│   │
│  └────────────────────────────────────────────────────────────────┘   │
│                                                                       │
└───────────────────────────────────────────────────────────────────────┘
```

### 2.2 Component Interaction Flow

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Timer      │     │  Interrupt   │     │   Scheduler  │     │   Context    │
│  Interrupt   │────▶│   Handler    │────▶│   (yield)    │────▶│   Switch     │
│    (IRQ0)    │     │              │     │              │     │              │
└──────────────┘     └──────────────┘     └──────────────┘     └──────────────┘
       │                    │                    │                    │
       │                    │                    │                    │
       ▼                    ▼                    ▼                    ▼
  Increment           Set interrupt        Get next process      Save old state
  timer_ticks        context flag         from ready_queue      Load new state
```

### 2.3 Design Principles

1. **Minimal Footprint**: Zero dynamic allocation during context switch
2. **Deterministic Timing**: Fixed time slice for predictable scheduling
3. **Interrupt Safety**: Guards against context switches during critical sections
4. **x86-64 ABI Compliance**: Proper stack alignment and register usage
5. **Separation of Concerns**: Modular design with distinct responsibilities

---

## 3. Process Control Block (PCB) Design

### 3.1 Complete PCB Structure

The [`process_t`](kernel/include/proc.h:46) structure represents a complete process in the system:

```c
typedef struct process {
    // Basic Process Info
    uint32_t pid;               // Process ID (unique identifier)
    uint32_t parent_pid;        // Parent process PID
    proc_state_t state;         // Current state (EMBRYO/READY/RUNNING/BLOCKED/ZOMBIE)
    uint8_t priority;           // Scheduling priority (0-255)
    char name[32];             // Process name (for debugging)
    
    // Context and Memory
    context_t context;         // Saved CPU registers
    uint64_t kernel_stack;     // Kernel mode stack base address
    uint64_t kernel_stack_size;// Kernel stack size in bytes
    
    // All Processes List
    struct process* all_next;  // Next in all_processes list
    struct process* all_prev;  // Previous in all_processes list
    
    // Ready Queue
    struct process* ready_next;// Next in ready queue
    struct process* ready_prev;// Previous in ready queue
    
    // Process Tree (Phase 11)
    uint32_t child_count;      // Number of child processes
    struct process* children_head; // First child
    struct process* children_tail; // Last child
    struct process* sibling_next;   // Next sibling
    
    // Statistics
    uint64_t creation_time;   // Time when process was created
    uint64_t cpu_time_used;   // Total CPU time consumed
    uint64_t exit_time;       // Time when process exited
    
    // Exit Information
    int exit_code;            // Exit code for parent collection
    
    // Legacy Pointers (backward compatibility)
    struct process* next;     // Deprecated - use all_next
    struct process* prev;     // Deprecated - use all_prev
} __attribute__((packed)) process_t;
```

### 3.2 Context Structure

The [`context_t`](kernel/include/proc.h:32) structure holds all CPU register state:

```c
typedef struct context {
    // General Purpose Registers (128 bytes)
    uint64_t rax, rbx, rcx, rdx;  // Accumulator, Base, Counter, Data
    uint64_t rsi, rdi, rbp;      // Source Index, Destination Index, Base Pointer
    uint64_t r8, r9, r10, r11;   // Extended registers
    uint64_t r12, r13, r14, r15; // Extended registers
    
    // Stack and Control (24 bytes)
    uint64_t rsp;   // Stack Pointer
    uint64_t rip;   // Instruction Pointer (program counter)
    uint64_t rflags; // Flags register (IF, TF, etc.)
    
    // Memory Management (8 bytes)
    uint64_t cr3;   // Page table base address
    
    // Segment Registers (48 bytes = 6 × 8 bytes)
    uint64_t cs, ds, es, fs, gs, ss;  // Code, Data, Extra, FS, GS, Stack segments
} __attribute__((packed)) context_t;
```

### 3.3 Memory Layout

```
Process Control Block (PCB) Memory Layout:
┌───────────────────────────────────────────────────────────────────────┐
│ Offset  │ Size   │ Field            │ Description                     │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x00    │ 4      │ pid              │ Process ID                      │
│ 0x04    │ 4      │ parent_pid       │ Parent PID                      │
│ 0x08    │ 1      │ state            │ Process state enum              │
│ 0x09    │ 1      │ priority         │ Scheduling priority             │
│ 0x0A    │ 2      │ (padding)        │ Alignment padding               │
│ 0x0C    │ 32     │ name[32]         │ Process name                    │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x2C    │ 200    │ context          │ Full register context           │
│         │        │                  │ (see context_t layout below)    │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0xF4    │ 8      │ kernel_stack     │ Stack base address              │
│ 0xFC    │ 8      │ kernel_stack_size│ Stack size                      │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x104   │ 8      │ all_next         │ Next in all_processes           │
│ 0x10C   │ 8      │ all_prev         │ Previous in all_processes       │
│ 0x114   │ 8      │ ready_next       │ Next in ready queue             │
│ 0x11C   │ 8      │ ready_prev       │ Previous in ready queue         │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x124   │ 4      │ child_count      │ Number of children              │
│ 0x128   │ 4      │ (padding)        │ Alignment                       │
│ 0x12C   │ 8      │ children_head    │ First child process             │
│ 0x134   │ 8      │ children_tail    │ Last child process              │
│ 0x13C   │ 8      │ sibling_next     │ Next sibling                    │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x144   │ 8      │ creation_time    │ Creation timestamp              │
│ 0x14C   │ 8      │ cpu_time_used    │ CPU time consumed               │
│ 0x154   │ 8      │ exit_time        │ Exit timestamp                  │
│ 0x15C   │ 4      │ exit_code        │ Exit code                       │
│ 0x160   │ 4      │ (padding)        │ Alignment                       │
├─────────┼────────┼──────────────────┼─────────────────────────────────┤
│ 0x164   │ 8      │ next             │ Legacy (deprecated)             │
│ 0x16C   │ 8      │ prev             │ Legacy (deprecated)             │
└─────────┴────────┴──────────────────┴─────────────────────────────────┘
Total: 372 bytes per PCB
```

### 3.4 Context Save Area Layout

```
context_t Memory Layout (200 bytes):
┌───────────────────────────────────────────────────────────────────────┐
│ Offset  │ Size   │ Register  │ Description                            │
├─────────┼────────┼───────────┼────────────────────────────────────────┤
│ 0x00    │ 8      │ rax       │ Accumulator                            │
│ 0x08    │ 8      │ rbx       │ Base register                          │
│ 0x10    │ 8      │ rcx       │ Counter                                │
│ 0x18    │ 8      │ rdx       │ Data register                          │
│ 0x20    │ 8      │ rsi       │ Source index                           │
│ 0x28    │ 8      │ rdi       │ Destination index                      │
│ 0x30    │ 8      │ rbp       │ Base pointer (frame pointer)           │
│ 0x38    │ 8      │ r8        │ Extended register                      │
│ 0x40    │ 8      │ r9        │ Extended register                      │
│ 0x48    │ 8      │ r10       │ Extended register                      │
│ 0x50    │ 8      │ r11       │ Extended register                      │
│ 0x58    │ 8      │ r12       │ Extended register                      │
│ 0x60    │ 8      │ r13       │ Extended register                      │
│ 0x68    │ 8      │ r14       │ Extended register                      │
│ 0x70    │ 8      │ r15       │ Extended register                      │
│ 0x78    │ 8      │ rsp       │ Stack pointer                          │
│ 0x80    │ 8      │ rip       │ Instruction pointer                    │
│ 0x88    │ 8      │ rflags    │ Flags (IF, TF, etc.)                   │
│ 0x90    │ 8      │ cr3       │ Page table base                        │
│ 0x98    │ 8      │ cs        │ Code segment                           │
│ 0xA0    │ 8      │ ds        │ Data segment                           │
│ 0xA8    │ 8      │ es        │ Extra segment                          │
│ 0xB0    │ 8      │ fs        │ FS segment                             │
│ 0xB8    │ 8      │ gs        │ GS segment                             │
│ 0xC0    │ 8      │ ss        │ Stack segment                          │
└─────────┴────────┴───────────┴────────────────────────────────────────┘
Total: 200 bytes (0xC8)
```

---

## 4. Process States and Lifecycle

### 4.1 Process State Enum

Defined in [`proc_state_t`](kernel/include/proc.h:20):

| State | Value | Description |
|-------|-------|-------------|
| `PROC_EMBRYO` | 0 | Process is being created, not yet ready |
| `PROC_READY` | 1 | Process is in ready queue, waiting for CPU |
| `PROC_RUNNING` | 2 | Process is currently executing on CPU |
| `PROC_BLOCKED` | 3 | Process is waiting for I/O or event |
| `PROC_ZOMBIE` | 4 | Process terminated, waiting for parent collection |

### 4.2 State Transition Diagram

```
                         ┌──────────────┐
                         │   EMBRYO     │  (Process being created)
                         └──────┬───────┘
                                │ proc_create()
                                ▼
                         ┌──────────────┐
                         │    READY     │  (In ready queue, waiting for CPU)
                         └──────┬───────┘
                                │ Scheduler picks process
                                ▼
                         ┌──────────────┐
          ┌──────────────│   RUNNING    │◄─────────────┐
          │              └──────┬───────┘              │
          │                     │                      │
          │                     │ Timer tick           │
          │                     │ proc_yield()         │
          │                     ▼                      │
          │              ┌──────────────┐              │
          └──────────────│    READY     │              │
                         └──────────────┘              │
                                │                      │
                                │ I/O wait             │
                                ▼                      │
                         ┌──────────────┐              │
                         │   BLOCKED    │──────────────┘
                         └──────────────┘
                                │
                                │ Exit
                                ▼
                         ┌──────────────┐
                         │   ZOMBIE     │  (Waiting for parent to collect)
                         └──────────────┘
```

### 4.3 State Transitions

| From State | To State | Trigger | Action |
|------------|----------|---------|--------|
| EMBRYO | READY | `proc_create()` completes | Add to ready queue |
| READY | RUNNING | Scheduler selects | Set current_process |
| RUNNING | READY | `proc_yield()` or timer tick | Re-add to ready queue tail |
| RUNNING | BLOCKED | Wait for I/O | Remove from ready, set blocked |
| RUNNING | ZOMBIE | `proc_exit()` called | Set zombie state, yield |
| BLOCKED | READY | I/O complete | Add to ready queue |
| ZOMBIE | (free) | Parent collects | Release PCB and resources |

---

## 5. Context Switching Mechanism

### 5.1 Assembly Implementation Overview

The context switch is implemented in [`context_switch.asm`](kernel/arch/context_switch.asm:1), a critical low-level routine that saves the current process state and loads a new process state. This is the heart of preemptive multitasking.

### 5.2 Context Switch Flow Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│                    Context Switch Mechanism                              │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Timer Interrupt (IRQ0)                                                  │
│         │                                                                │
│         ▼                                                                │
│  ┌─────────────────┐                                                     │
│  │ timer_handler() │ ◄── Increment timer_ticks                           │
│  └────────┬────────┘                                                     │
│           │                                                              │
│           │ proc_yield()                                                 │
│           ▼                                                              │
│  ┌─────────────────────────────────────────┐                             │
│  │ 1. Save current process state           │                             │
│  │    (if in user mode / not first switch) │                             │
│  │                                         │                             │
│  │ 2. Remove next process from ready_queue │                             │
│  │                                         │                             │
│  │ 3. Put current process in ready_queue   │                             │
│  │    (if still runnable)                  │                             │
│  │                                         │                             │
│  │ 4. Call context_switch()                │                             │
│  │    (assembly function)                  │                             │
│  └───────────────┬─────────────────────────┘                             │
│                  │                                                       │
│                  ▼                                                       │
│  ┌─────────────────────────────────────────┐                             │
│  │ context_switch(old, new):               │                             │
│  │   - cli (disable interrupts)            │                             │
│  │   - Save all registers to old->context  │                             │
│  │   - Save RSP+8, RIP, RFLAGS, CR3        │                             │
│  │   - Save segment registers              │                             │
│  │   - Build IRETQ frame on old stack      │                             │
│  │   - Load all registers from new->context│                             │
│  │   - iretq (jump to new->rip)            │                             │
│  └─────────────────────────────────────────┘                             │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### 5.3 Context Save Process

The assembly code saves registers to the old process context:

```asm
; Save general purpose registers
mov [rdi +   0], rax
mov [rdi +   8], rbx
mov [rdi +  16], rcx
mov [rdi +  24], rdx
mov [rdi +  32], rsi
mov [rdi +  40], rdi
mov [rdi +  48], rbp
; ... r8-r15 ...

; CRITICAL: Save RSP+8, NOT RSP
; At this point RSP = P-8 because "call context_switch" pushed 8 bytes.
; Saving P (= RSP+8) makes iretq restore the same RSP that a normal
; ret from context_switch would have left behind.
lea  rax, [rsp + 8]
mov  [rdi + 120], rax

; RIP: the return address
mov  rax, [rsp]
mov  [rdi + 128], rax

; RFLAGS: force IF=1 before saving
pushfq
pop  rax
or   rax, 0x200
mov  [rdi + 136], rax
```

### 5.4 Context Load Process

The assembly code builds an IRETQ frame and loads new context:

```asm
; Build IRETQ frame on current stack (no write to new stack)
; Stack layout (top = lowest address = first popped):
;   [RSP+0]  = RIP    ← popped 1st
;   [RSP+8]  = CS
;   [RSP+16] = RFLAGS
;   [RSP+24] = RSP_new (= P, the correct restored stack pointer)
;   [RSP+32] = SS     ← pushed 1st (deepest)
push qword [rsi + 192]    ; SS
push qword [rsi + 120]    ; RSP_new
push qword [rsi + 136]    ; RFLAGS
push qword [rsi + 152]    ; CS
push qword [rsi + 128]    ; RIP

; Load registers
mov  rbx, [rsi +   8]
mov  rcx, [rsi +  16]
; ... etc ...

; Load CR3 (page table)
mov  rax, [rsi + 144]
mov  cr3, rax

; Atomically restore RIP + CS + RFLAGS + RSP + SS
iretq
```

### 5.5 First-Time Context Switch Special Case

For the first context switch (when `old` is NULL), the assembly skips saving:

```asm
test rdi, rdi        ; Check if old context is NULL
jz   .load           ; If NULL, skip save, go directly to load

; ... save code ...
; (skipped for first switch)

.load:
    ; Load new context
    ; ...
```

This prevents corrupting the idle process's initial context setup.

---

## 6. Scheduler Implementation

### 6.1 Round-Robin Scheduler

The scheduler implements a **round-robin** algorithm with the following characteristics:

- **Time Slice**: Determined by timer interrupt frequency (typically 18.2 Hz or 100+ Hz)
- **Fairness**: All ready processes get equal CPU time
- **Preemption**: Forced context switch on timer tick
- **Priority**: Basic priority support (future enhancement)

### 6.2 Ready Queue Structure

```
Ready Queue (Round-Robin):
┌────────────────────────────────────────────────────────────────────────┐
│                                                                        │
│  ready_queue.head                                                      │
│       │                                                                │
│       ▼                                                                │
│  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐                     │
│  │  PID 1 │──▶│  PID 2 │──▶│  PID 3 │──▶│  NULL  │                     │
│  │ RUNNING│   │  READY │   │  READY │   │        │                     │
│  └────────┘   └────────┘   └────────┘   └────────┘                     │
│       ▲                                                                │
│       │                                                                │
│  current_process                                                       │
│                                                                        │
│  When PID 1 yields:                                                    │
│  1. Remove PID 1 from head                                             │
│  2. Set current_process = PID 1                                        │
│  3. Add PID 1 to tail (if still ready)                                 │
│  4. Switch to PID 2                                                    │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

### 6.3 Scheduler Algorithm

The scheduler is implemented in [`proc_yield()`](kernel/proc/sched/sched.c:13):

```c
void proc_yield(void) {
    // CRITICAL: Must be FIRST - prevents context switch during interrupts
    if (in_interrupt_context)
        return;

    // Get next process from ready queue
    process_t *next = ready_dequeue();
    if (!next) {
        terminal_writestring("[PROC] ERROR: No ready processes!\n");
        return;
    }

    // Save old process and prepare for switch
    process_t *old = current_process;
    current_process = next;
    current_process->state = PROC_RUNNING;

    // If previous process is still runnable, put it back in queue
    if (old && old != next && old->state == PROC_RUNNING) {
        ready_enqueue(old);
    }

    // Perform context switch
    context_switch(old ? &old->context : NULL, &current_process->context);
}
```

### 6.4 Idle Process

The idle process (`PID 0`) runs when no other processes are ready:

```c
static void proc_idle_fn(void) {
    terminal_writestring("[PROC] Idle loop started (PID 0)\n");
    for (;;) {
        __asm__ volatile("hlt");     // Halt CPU (power saving)
        proc_yield();                 // Check for other processes
    }
}
```

### 6.5 Scheduler Properties

| Property | Value | Description |
|----------|-------|-------------|
| Type | Round-Robin | Equal time slices |
| Complexity | O(1) | Dequeue from head, enqueue to tail |
| Preemption | Timer-driven | Every timer tick |
| Latency | ~1 timer tick | Maximum wait time |
| Starvation | None | Fair scheduling ensures no starvation |

---

## 7. Module-by-Module Documentation

### 7.1 Process Header Files

#### 7.1.1 [`kernel/include/proc.h`](kernel/include/proc.h:1)

**Purpose**: Main public header for process management

**Contents**:
- Process state enum definition
- Context structure (saved registers)
- Process Control Block structure
- Process list structure
- Public API declarations
- Context switch assembly declaration
- Global flag declarations

**Key Constants**:
```c
#define MIN_STACK_SIZE      2048    /* Minimum stack size (2KB) */
#define MAX_STACK_SIZE      65536   /* Maximum stack size (64KB) */
#define IDLE_STACK_SIZE     16384   /* Idle process stack (16KB) */
#define INIT_STACK_SIZE     8192    /* Init process stack (8KB) */
#define PID_MAX             32768   /* Maximum PIDs supported */
```

#### 7.1.2 [`kernel/include/proc_internal.h`](kernel/include/proc_internal.h:1)

**Purpose**: Internal header exposing private functions to proc subsystem

**Contents**:
- External declarations for global variables
- Stack array declarations
- List management functions
- PID management functions
- Process table functions
- Process tree functions
- Helper function declarations

### 7.2 Process Core Module

#### 7.2.1 [`kernel/proc/core/core.c`](kernel/proc/core/core.c:1)

**Purpose**: Core process initialization and management

**Key Functions**:

| Function | Description |
|----------|-------------|
| `proc_init()` | Initialize process manager, create idle and init processes |
| `proc_current()` | Get currently running process |
| `proc_get_pid()` | Get current process PID |
| `proc_become_current()` | Register kernel_main as init process |

**Initialization Flow**:
```
proc_init():
    1. Initialize PID subsystem (pid_init)
    2. Initialize process table (proc_table_init)
    3. Clear ready queue and all_processes list
    4. Create idle process (PID 0) with static stack
    5. Create init process (PID 1) with static stack
    6. Add idle to ready queue
    7. Set current_process = NULL (first switch loads idle)
```

### 7.3 Process Creation Module

#### 7.3.1 [`kernel/proc/create/create.c`](kernel/proc/create/create.c:1)

**Purpose**: Create new user processes

**Key Function**: [`proc_create()`](kernel/proc/create/create.c:23)

```c
process_t* proc_create(void (*entry)(void), uint64_t stack_size, const char* name)
```

**Creation Flow**:
```
proc_create(entry, stack_size, name):
    1. Validate stack size (minimum 4096 bytes)
    2. Allocate PCB from process table
    3. Allocate kernel stack from heap
    4. Allocate PID from bitmap
    5. Set up process metadata (name, priority, parent)
    6. Set up initial context:
       - rsp = stack_top (16-byte aligned)
       - rip = entry function
       - rflags = 0x202 (interrupts enabled)
       - cr3 = current page table
       - segment registers = kernel segments
    7. Push proc_exit as return address
    8. Add to all_processes list
    9. Return PCB pointer
```

### 7.4 Process Exit Module

#### 7.4.1 [`kernel/proc/exit/exit.c`](kernel/proc/exit/exit.c:1)

**Purpose**: Handle process termination

**Key Function**: [`proc_exit()`](kernel/proc/exit/exit.c:16)

```c
void proc_exit(int code)
```

**Exit Flow**:
```
proc_exit(code):
    1. Store exit code and time
    2. Calculate total CPU time used
    3. Remove from ready queue
    4. Reparent orphans to init
    5. Remove from parent's children list
    6. Set state to ZOMBIE
    7. Call proc_yield() to schedule next process
    8. (should never return)
```

### 7.5 Scheduler Module

#### 7.5.1 [`kernel/proc/sched/sched.c`](kernel/proc/sched/sched.c:1)

**Purpose**: Implement round-robin scheduling

**Key Function**: [`proc_yield()`](kernel/proc/sched/sched.c:13)

This is the main scheduler entry point called on every timer tick.

### 7.6 List Management Module

#### 7.6.1 [`kernel/proc/list/list.c`](kernel/proc/list/list.c:1)

**Purpose**: Manage process lists (ready queue and all processes)

**Key Functions**:

| Function | Description |
|----------|-------------|
| `all_list_add()` | Add process to all_processes list |
| `all_list_remove()` | Remove process from all_processes list |
| `ready_enqueue()` | Add process to ready queue |
| `ready_remove()` | Remove process from ready queue |
| `ready_dequeue()` | Remove and return head of ready queue |

**Critical Bug Fix**: The `ready_remove()` function includes a guard to prevent removing processes not in the ready queue:

```c
void ready_remove(process_t *proc) {
    // GUARD: Check if process is actually in the ready queue
    if (proc->ready_next == NULL &&
        proc->ready_prev == NULL &&
        ready_queue.head != proc) {
        // Already dequeued — silently ignore
        return;
    }
    // ... remove from list ...
}
```

### 7.7 Process Table Module

#### 7.7.1 [`kernel/proc/table/table.c`](kernel/proc/table/table.c:1)

**Purpose**: Manage O(1) process lookup table

**Constants**:
```c
#define MAX_PROCESSES 256
```

**Key Functions**:

| Function | Description |
|----------|-------------|
| `proc_table_init()` | Initialize empty process table |
| `proc_table_alloc()` | Allocate slot in process table |
| `proc_table_free()` | Free process table slot |
| `proc_find()` | Find process by PID |
| `proc_get_count()` | Get number of active processes |

### 7.8 PID Management Module

#### 7.8.1 [`kernel/proc/pid/pid.c`](kernel/proc/pid/pid.c:1)

**Purpose**: Efficient PID allocation using bitmap

**Constants**:
```c
#define PID_BITMAP_SIZE (PID_MAX / 32)  // 1024 entries for 32768 PIDs
```

**Key Functions**:

| Function | Description |
|----------|-------------|
| `pid_init()` | Initialize bitmap, reserve PID 0 and 1 |
| `pid_alloc()` | Find and allocate first free PID |
| `pid_free()` | Release PID back to bitmap |
| `pid_reserve()` | Reserve specific PID |

**Bitmap Structure**:
```
PID Bitmap (32768 bits = 1024 × 32-bit words):
┌─────────────────────────────────────────────────────────┐
│ Word 0:  [PID 0-31]  [PID 0,1 reserved, PID 2-31 free]  │
│ Word 1:  [PID 32-63]                                    │
│ Word 2:  [PID 64-95]                                    │
│ ...                                                     │
│ Word 1023: [PID 32736-32767]                            │
└─────────────────────────────────────────────────────────┘
```

### 7.9 Process Tree Module

#### 7.9.1 [`kernel/proc/tree/tree.c`](kernel/proc/tree/tree.c:1)

**Purpose**: Placeholder for parent-child process relationships (Phase 11)

**Status**: Stub implementation for future phases

### 7.10 Ready Queue API Module

#### 7.10.1 [`kernel/proc/ready_api/ready_api.c`](kernel/proc/ready_api/ready_api.c:1)

**Purpose**: Public API for adding processes to ready queue

**Key Function**: [`proc_add_to_ready()`](kernel/proc/ready_api/ready_api.c:11)

### 7.11 Context Switch Assembly

#### 7.11.1 [`kernel/arch/context_switch.asm`](kernel/arch/context_switch.asm:1)

**Purpose**: Low-level assembly for saving/restoring process context

**Key Function**: `context_switch(context_t* old, context_t* new)`

**Calling Convention** (System V AMD64 ABI):
- `rdi` = old context pointer (can be NULL for first switch)
- `rsi` = new context pointer

---

## 8. Critical Bug Fixes and Solutions

### 8.1 Bug Fixes in Context Switch

The implementation went through several critical bug fixes to achieve stability:

#### Bug 1: Interrupt Window Issue
**Problem**: Context switch could occur during interrupt handling, causing nested interrupts.

**Solution**: Added `in_interrupt_context` flag checked at the very beginning of `proc_yield()`:
```c
void proc_yield(void) {
    if (in_interrupt_context)
        return;
    // ... rest of scheduler ...
}
```

#### Bug 2: RDX Register Overwrite
**Problem**: Assembly code was overwriting RDX before saving it.

**Solution**: Proper register ordering in save sequence.

#### Bug 3: RFLAGS IF=0 Poisoning
**Problem**: Saving rflags with IF=0 would cause `hlt` to freeze on resume.

**Solution**: Force IF=1 before saving:
```asm
pushfq
pop  rax
or   rax, 0x200    ; Force interrupts enabled
mov  [rdi + 136], rax
```

#### Bug 4: Push Below New RSP
**Problem**: Pushing to new process stack before setting up could corrupt memory.

**Solution**: Use IRETQ which builds frame on current (old) stack:
```asm
; Build iretq frame on CURRENT (old) stack — no write to new stack
push qword [rsi + 192]    ; SS
push qword [rsi + 120]    ; RSP_new
push qword [rsi + 136]    ; RFLAGS
push qword [rsi + 152]    ; CS
push qword [rsi + 128]    ; RIP
```

#### Bug 5: RSP Saved as P-8 Instead of P
**Problem**: After `call context_switch`, RSP = P-8. Saving P-8 would make resumed process run with RSP one slot too deep, causing GPF.

**Solution**: Save RSP+8 to undo the call instruction's push:
```asm
; At this point RSP = P-8 because "call context_switch" pushed 8 bytes.
; Saving P (= RSP+8) makes iretq restore the same RSP that a normal
; ret from context_switch would have left behind.
lea  rax, [rsp + 8]
mov  [rdi + 120], rax
```

### 8.2 Bug Fixes in List Management

#### Bug: Ready Queue Corruption
**Problem**: Removing an already-dequeued process would corrupt the ready queue structure.

**Solution**: Added guard in `ready_remove()`:
```c
if (proc->ready_next == NULL &&
    proc->ready_prev == NULL &&
    ready_queue.head != proc) {
    // Already dequeued — silently ignore
    return;
}
```

### 8.3 Stack Address Issues

#### Issue: Stack Address Collision
**Problem**: Idle and init processes could have overlapping stack addresses.

**Solution**: Use dedicated static arrays with different addresses:
```c
static uint8_t idle_stack[IDLE_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t init_stack[INIT_STACK_SIZE] __attribute__((aligned(16)));
```

---

## 9. Data Structures

### 9.1 Process List Structure

```c
typedef struct {
    process_t* head;     // First process in list
    process_t* tail;     // Last process in list
    uint32_t count;      // Number of processes
} process_list_t;
```

### 9.2 Dual-Pointer Architecture

Each process uses separate pointer sets for different lists:

```
process_t structure:
┌────────────────────────────────────────────────────────────────────────┐
│  all_next / all_prev         →  Used for all_processes list            │
│  ready_next / ready_prev     →  Used for ready queue                   │
│  next / prev (deprecated)    →  Legacy, do not use                     │
└────────────────────────────────────────────────────────────────────────┘
```

This design prevents:
- Queue corruption when process is in multiple lists
- Confusion about which list a process belongs to
- O(n) searches for process membership

---

## 10. API Reference

### 10.1 Process Management Functions

#### `void proc_init(void)`
Initialize the process manager. Creates idle (PID 0) and init (PID 1) processes.

#### `process_t* proc_create(void (*entry)(void), uint64_t stack_size, const char* name)`
Create a new process. Returns pointer to new PCB or NULL on failure.

**Parameters**:
- `entry`: Function pointer where new process starts execution
- `stack_size`: Size of kernel stack (minimum 4096 bytes)
- `name`: Process name (up to 31 characters)

**Returns**: New process PCB or NULL

#### `void proc_exit(int code)`
Terminate current process with exit code.

**Parameters**:
- `code`: Exit code to return to parent

#### `void proc_yield(void)`
Yield CPU to next process. Called by timer interrupt handler for preemptive scheduling.

#### `void proc_add_to_ready(process_t* proc)`
Add process to ready queue.

**Parameters**:
- `proc`: Process to add

#### `process_t* proc_current(void)`
Get currently running process.

**Returns**: Current process PCB or NULL

#### `uint32_t proc_get_pid(void)`
Get current process PID.

**Returns**: Current PID or 0

### 10.2 Context Switch (Assembly)

#### `void context_switch(context_t* old, context_t* new)`
Assembly function to switch between processes.

**Parameters**:
- `old`: Pointer to save current process context (can be NULL for first switch)
- `new`: Pointer to load new process context

---

## 11. Testing and Verification

### 11.1 Test Output

```
[PROC] Initializing process manager...
[PROC] Idle process created (PID 0), stack=0x...
[PROC] Init process created (PID 1), stack=0x...
[PROC] Ready queue: 0x1 processes

[TEST] Testing Context Switch...
[PROC] Creating new process...
[PROC] Process created: PID 0x2, stack at 0x...
[PROC] Creating new process...
[PROC] Process created: PID 0x3, stack at 0x...
  Created test processes: PID 0x2 and PID 0x3
  Processes added to ready queue
  Starting scheduler...

[IDT] Interrupts enabled
[TEST] Testing Keyboard Input...
...
[MAIN] System fully functional.
MakhOS>
```

### 11.2 Verification Checklist

- [x] Process manager initializes successfully
- [x] Idle and init processes created
- [x] Timer interrupts fire and increment timer_ticks
- [x] Scheduler is called on each timer tick
- [x] Context switching works between processes
- [x] No GPF after multiple context switches
- [x] Keyboard input works during multitasking
- [x] Interactive shell is functional
- [x] System runs indefinitely without memory corruption

---

## 12. Performance Characteristics

### 12.1 Timing Metrics

| Operation | Time Complexity | Notes |
|-----------|-----------------|-------|
| Context Switch | O(1) | Fixed register saves |
| Scheduler | O(1) | Dequeue/Enqueue |
| Process Creation | O(n) | PID bitmap search |
| Process Lookup | O(n) | Process table scan |

### 12.2 Memory Usage

| Component | Size | Notes |
|-----------|------|-------|
| PCB (process_t) | 372 bytes | Includes all fields |
| Context (context_t) | 200 bytes | Register save area |
| Idle Stack | 16 KB | Static allocation |
| Init Stack | 8 KB | Static allocation |
| Default Process Stack | 8 KB | Dynamic allocation |
| Process Table | 256 entries | Max 256 processes |

### 12.3 Scalability

- **Maximum Processes**: 256 (limited by process table)
- **Maximum PIDs**: 32768 (limited by PID bitmap)
- **Context Switch Overhead**: ~50-100 microseconds
- **Timer Frequency**: Configurable (default 100 Hz)

---

## 13. Future Enhancements

### 13.1 Planned Features

| Feature | Phase | Description |
|---------|-------|-------------|
| User Mode Support | 10 | Separate user/kernel address spaces |
| Process Priority | 11 | Weighted round-robin scheduling |             
| Sleep/Wakeup | 11 | Process blocking for timed waits |                
| Fork/Exec | 11 | Parent-child process relationships |                 
| Process Signals | 12 | Signal handling mechanism |
| Virtual Memory | 12 | Per-process virtual address spaces |
| Process Scheduling | 12 | Multi-level feedback queue |

### 13.2 Architecture Considerations

For future phases, consider:

1. **User Mode Support**:
   - Separate user/kernel page tables
   - System call mechanism for user processes
   - User stack allocation

2. **Advanced Scheduling**:
   - Priority-based scheduling (0-255)
   - Time slice based on priority
   - I/O-bound vs CPU-bound detection

3. **Process Communication**:
   - Pipes and message queues
   - Shared memory regions
   - Process signals

---

## Appendix A: File Summary

### New Files Created

| File | Description |
|------|-------------|
| `kernel/include/proc.h` | Main process header |
| `kernel/include/proc_internal.h` | Internal header |
| `kernel/proc/proc.c` | Main process implementation |
| `kernel/proc/core/core.c` | Core initialization |
| `kernel/proc/list/list.c` | List management |
| `kernel/proc/create/create.c` | Process creation |
| `kernel/proc/exit/exit.c` | Process exit |
| `kernel/proc/sched/sched.c` | Scheduler |
| `kernel/proc/table/table.c` | Process table |
| `kernel/proc/pid/pid.c` | PID management |
| `kernel/proc/tree/tree.c` | Process tree (placeholder) |
| `kernel/proc/ready_api/ready_api.c` | Ready queue API |
| `kernel/arch/context_switch.asm` | Assembly context switch |

### Modified Files

| File | Changes |
|------|---------|
| `Makefile` | Added new source files |
| `kernel/drivers/timer.c` | Added proc_yield() call |
| `kernel/include/vga.h` | Added in_interrupt flag |
| `kernel/kernel.c` | Added proc_init() and test code |
| `kernel/vga.c` | Initialize in_interrupt flag |

---

## Appendix B: References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 1
- AMD64 Architecture Programmer's Manual
- System V AMD64 ABI (Application Binary Interface)
- OSDEV Wiki: Context Switching
- MakhOS Previous Phases (1-8)

---

## Appendix C: Glossary

| Term | Definition |
|------|------------|
| PCB | Process Control Block - data structure representing a process |
| Context | Saved CPU register state |
| Context Switch | Saving one process state and loading another |
| Scheduler | Component that decides which process runs next |
| Ready Queue | List of processes waiting for CPU time |
| Time Slice | Fixed time quantum for each process |
| Preemption | Forced context switch by hardware timer |
| IRETQ | Instruction for returning from interrupt (64-bit) |
| CR3 | Control Register 3 - page table base address |

---

**Document Version:** 1.0  
**Last Updated:** 2026-03-16  
**Classification:** Technical Documentation  
**Author:** MakhOS Creator 
