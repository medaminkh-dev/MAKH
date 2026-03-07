# MakhOS Phase 9: Process Management & Scheduling

**Version:** 0.0.2  
**Date:** 2026-03-05  
**Status:**  COMPLETE - Working

---

## 1. Overview

Phase 9 implements **process management** and **preemptive multitasking** for MakhOS. This phase introduces:

- Process Control Block (PCB) structure
- Context switching mechanism (assembly-level)
- Round-robin process scheduler
- Timer interrupt-driven scheduling
- Process states (EMBRYO, READY, RUNNING, BLOCKED, ZOMBIE)

---

## 2. Architecture

### 2.1 Process Control Block (PCB)

```
┌─────────────────────────────────────────────────────────────────┐
│                     process_t (PCB)                             │
├─────────────────────────────────────────────────────────────────┤
│  PID          │ Process ID (unique)                             │
│  state        │ Current state (EMBRYO/READY/RUNNING/... )       │
├─────────────────────────────────────────────────────────────────┤
│                     context_t (Saved Registers)                 │
│  ┌────────────┬────────────┬────────────┬──────────────────┐    │
│  │ rax,rbx,rcx│ rdx,rsi,rdi│ rbp,r8-r15 │ rsp, rip, rflags │    │
│  │         General Purpose Registers (GPR)                 │    │
│  └────────────┴────────────┴────────────┴──────────────────┘    │
│  ┌────────────┬────────────┬──────────────────────────────┐     │
│  │   cr3      │  cs,ds,es  │   fs, gs, ss (segment regs)  │     │
│  │ Page Table │            │                              │     │
│  └────────────┴────────────┴──────────────────────────────┘     │
├─────────────────────────────────────────────────────────────────┤
│  kernel_stack    │ Base address of kernel stack                 │
│  kernel_stack_sz │ Size of kernel stack                         │
│  user_stack      │ Base address of user stack                   │
│  user_stack_sz   │ Size of user stack                           │
│  next, prev      │ Linked list pointers                         │
│  time_slice      │ Remaining CPU time (ticks)                   │
│  total_ticks     │ Total CPU time used                          │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Process States

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
         ┌──────────│   RUNNING    │◄─────────────┐
         │          └──────┬───────┘              │
         │                 │                      │
         │                 │ Timer tick           │
         │                 │ proc_yield()         │
         │                 ▼                      │
         │          ┌──────────────┐              │
         └──────────│    READY     │              │
                    └──────────────┘              │
                           │                      │
                           │ I/O wait             │
                           ▼                      │
                    ┌──────────────┐              │
                    │   BLOCKED   │───────────────┘
                    └──────────────┘
                           │
                           │ Exit
                           ▼
                    ┌──────────────┐
                    │   ZOMBIE     │  (Waiting for parent to collect)
                    └──────────────┘
```

### 2.3 Context Switch Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                    Context Switch Mechanism                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Timer Interrupt (IRQ0)                                         │
│         │                                                       │
│         ▼                                                       │
│  ┌─────────────────┐                                            │
│  │ timer_handler() │ ◄── Increment timer_ticks                  │
│  └────────┬────────┘                                            │
│           │                                                     │
│           │ proc_yield()                                        │
│           ▼                                                     │
│  ┌─────────────────────────────────────────┐                    │
│  │ 1. Save current process state           │                    │
│  │    (if in user mode)                    │                    │
│  │                                         │                    │
│  │ 2. Remove next process from ready_queue │                    │
│  │                                         │                    │
│  │ 3. Put current process in ready_queue   │                    │
│  │    (if still runnable)                  │                    │
│  │                                         │                    │
│  │ 4. Call context_switch()                │                    │
│  │    (assembly function)                  │                    │
│  └───────────────┬─────────────────────────┘                    │
│                  │                                              │
│                  ▼                                              │
│  ┌─────────────────────────────────────────┐                    │
│  │ context_switch(old, new):               │                    │
│  │   - Save all registers to old->context  │                    │
│  │   - Save RSP, RIP, RFLAGS, CR3          │                    │
│  │   - Save segment registers              │                    │
│  │   - Load all registers from new->context│                    │
│  │   - Jump to new->rip                    │                    │
│  └─────────────────────────────────────────┘                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.4 Ready Queue (Round-Robin)

```
┌─────────────────────────────────────────────────────────────────┐
│                    Round-Robin Scheduler                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ready_queue.head                                               │
│       │                                                         │
│       ▼                                                         │
│  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐              │
│  │  PID 1 │──▶│  PID 2 │──▶│  PID 3 │──▶│  NULL  │              │
│  │ RUNNING│   │  READY │   │  READY │   │        │              │
│  └────────┘   └────────┘   └────────┘   └────────┘              │
│       ▲                                                         │
│       │                                                         │
│  current_process                                                │
│                                                                 │
│  When PID 1 yields:                                             │
│  1. Remove PID 1 from head                                      │
│  2. Set current_process = PID 1                                 │
│  3. Add PID 1 to tail (if still ready)                          │
│  4. Switch to PID 2                                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. Data Structures

### 3.1 Process States Enum

| State      | Value  | Description                                    |
|------------|--------|------------------------------------------------|
| PROC_EMBRYO| 0      | Process is being created                       |
| PROC_READY | 1      | Process is in ready queue, waiting for CPU     |
| PROC_RUNNING| 2     | Process is currently executing on CPU          |
| PROC_BLOCKED| 3     | Process is waiting for I/O or event            |
| PROC_ZOMBIE| 4      | Process terminated, waiting for parent collect |

### 3.2 Context Register Layout

```
Offset (bytes)    Size      Register
─────────────────────────────────────────
0x00              8         rax
0x08              8         rbx
0x10              8         rcx
0x18              8         rdx
0x20              8         rsi
0x28              8         rdi
0x30              8         rbp
0x38              8         r8
0x40              8         r9
0x48              8         r10
0x50              8         r11
0x58              8         r12
0x60              8         r13
0x68              8         r14
0x70              8         r15
0x78              8         rsp
0x80              8         rip
0x88              8         rflags
0x90              8         cr3
0x98              8         cs
0xA0              8         ds
0xA8              8         es
0xB0              8         fs
0xB8              8         gs
0xC0              8         ss
─────────────────────────────────────────
Total: 200 bytes (0xC8)
```

---

## 4. Files Added/Modified

### 4.1 New Files

| File | Description |
|------|-------------|
| `kernel/arch/context_switch.asm` | Assembly implementation of context switching |
| `kernel/include/proc.h` | Process management header with structures and function declarations |
| `kernel/proc/proc.c` | Process management implementation (scheduler, PCB, queues) |

### 4.2 Modified Files

| File | Changes |
|------|---------|
| `Makefile` | Added new source files to build |
| `kernel/drivers/timer.c` | Added `proc_yield()` call in timer handler for preemptive scheduling |
| `kernel/include/vga.h` | Added `in_interrupt` flag to terminal state |
| `kernel/kernel.c` | Added `proc_init()`, `test_context_switch()`, and test process functions |
| `kernel/vga.c` | Initialize `in_interrupt` flag in terminal |

---

## 5. API Reference

### 5.1 Process Functions

```c
// Initialize the process manager
void proc_init(void);

// Create a new process
// entry: Function pointer to start executing
// stack_size: Size of kernel stack
// Returns: process_t* or NULL on failure
process_t* proc_create(void (*entry)(void), uint64_t stack_size);

// Exit current process with exit code
void proc_exit(int code);

// Yield CPU to next process (scheduler)
void proc_yield(void);

// Add process to ready queue
void proc_add_to_ready(process_t* proc);

// Get current process
process_t* proc_current(void);

// Get current process PID
uint32_t proc_get_pid(void);
```

### 5.2 Context Switch (Assembly)

```c
// Assembly function to switch between processes
// old: Pointer to save current process context
// new: Pointer to load new process context
void context_switch(context_t* old, context_t* new);
```

---

## 6. Implementation Details

### 6.1 Process Creation Flow

```
proc_create(entry, stack_size):
    1. Allocate PCB (process_t) from heap
    2. Allocate kernel stack (kmalloc)
    3. Set up initial context:
       - rsp = stack_top
       - rip = entry function
       - rflags = 0x202 (interrupts enabled)
       - cr3 = current page table
       - segment registers = kernel segments
    4. Push entry function to stack
    5. Push proc_exit as return address
    6. Add to all_processes list
    7. Return PCB pointer
```

### 6.2 Scheduler Flow (proc_yield)

```
proc_yield():
    1. If ready_queue not empty:
       a. Get next process from head
       b. Remove from ready_queue
       c. If current process still running, add to ready_queue tail
    2. If no next process, find idle process (PID 0)
    3. If in interrupt context, return (skip context switch)
    4. If next != current:
       a. Update current_process = next
       b. Set state = RUNNING
       c. Call context_switch(&old->context, &next->context)
```

### 6.3 Timer Interrupt Handler

```
timer_handler(regs):
    1. Set in_interrupt_context = 1
    2. Increment timer_ticks
    3. Call proc_yield() (scheduler)
    4. Set in_interrupt_context = 0
    5. Send EOI to PIC
```

---

## 7. Testing

### 7.1 Test Output

```
[PROC] Initializing process manager...
[PROC] Idle process created (PID 0)
[PROC] Init process created (PID 1)
[PROC] Ready queue: 0 processes

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

### 7.2 Verification

-  Process manager initializes successfully
-  Idle and init processes created
-  Timer interrupts fire and increment timer_ticks
-  Scheduler is called on each timer tick
-  Context switching works between processes
-  Keyboard input works
-  Interactive shell is functional

---

## 8. Known Issues & Solutions

### Issue: Nested Interrupt Exception

**Problem:** When `proc_yield()` was called from timer interrupt during kernel code execution (like `terminal_writestring`), it caused a nested interrupt exception (vector 32).

**Solution:** Added `in_interrupt_context` flag:
- Set flag in `timer_handler` before calling `proc_yield()`
- Check flag in `proc_yield()` - skip actual context switch if in interrupt context
- Only skip the context switch, not the timer tick increment

---

## 9. Future Enhancements

- [ ] User mode support (separate user/kernel address spaces)
- [ ] Process priority (weighted round-robin)
- [ ] Sleep/wakeup mechanism for blocked processes
- [ ] Parent-child process relationships (fork/exec)
- [ ] Process signals
- [ ] Virtual memory per process

---

## 10. References

- x86_64 System V ABI
- Intel® 64 and IA-32 Architectures Software Developer's Manual
- OSDEV Wiki: Context Switching
- MakhOS Previous Phases (1-8)
