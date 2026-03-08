# MakhOS Phase 12: fork() System Call

**Version:** 0.0.3  
**Date:** 2026-03-08  
**Status:** COMPLETE - Working

---

## 1. Overview

Phase 12 implements the **`fork()`** system call - one of the most fundamental operations in Unix-like operating systems. This allows a process to create an exact copy of itself.

### What is fork()?

The `fork()` system call creates a new process (child) that is an exact copy of the parent process. After fork:
- **Parent** receives the child's PID (positive number)
- **Child** receives 0 (indicating it's the child)
- Both processes continue execution from the same point

---

## 2. How fork() Works - Step by Step

### 2.1 Fork Flow Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         FORK() SYSTEM CALL FLOW                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   User Process (Parent)                                                     │
│   ┌────────────────┐                                                        │
│   │  ...           │                                                        │
│   │  pid = fork()  │  ◄── Parent calls fork()                               │
│   └───────┬────────┘                                                        │
│           │                                                                 │
│           ▼                                                                 │
│   ┌──────────────────────────────────────────────────────────────────┐      │
│   │                    KERNEL: sys_fork()                            │      │
│   │  1. Get current process (parent)                                 │      │
│   │  2. Allocate new PID from bitmap                                 │      │
│   │  3. Allocate process table slot                                  │      │
│   │  4. Copy kernel stack                                            │      │
│   │  5. Copy context (set child's RAX = 0)                           │      │
│   │  6. Add child to scheduler queue                                 │      │
│   │  7. Return child's PID to parent                                 │      │
│   └──────────────────────────────────────────────────────────────────┘      │
│           │                                                                 │
│           ▼                                                                 │
│   ┌────────────────┐      ┌────────────────────────────────────────┐        │
│   │ Parent:        │      │  Child:                                │        │
│   │ RAX = child_PID│      │  RAX = 0          (same code path!)    │        │
│   │ (e.g., 5)     │      │  Same execution context                 │        │
│   └────────────────┘      └────────────────────────────────────────┘        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Detailed Algorithm

```
proc_fork():
    1. parent = proc_current()
    2. IF parent == NULL: RETURN -1
    
    3. child = proc_table_alloc()
    4. IF child == NULL: RETURN -1
    
    5. child->pid = proc_alloc_pid()
    6. IF child->pid < 0: 
           proc_table_free(child)
           RETURN -1
    
    7. child->priority = parent->priority
    8. child->parent_pid = parent->pid
    9. child->state = PROC_READY
    
   10. copy_kernel_stack(parent, child)
   11. copy_user_memory(parent, child)
   
   12. child->context = parent->context
   13. child->context.rax = 0        // CHILD GETS 0!
   
   14. proc_add_to_ready(child)     // Add to scheduler
   15. proc_add_child(parent, child) // Update process tree
   
   16. RETURN child->pid             // PARENT GETS CHILD'S PID
```

---

## 3. Key Implementation Details

### 3.1 Memory Copy

The fork implementation uses two helper functions:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    KERNEL STACK COPY                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Parent Stack              Child Stack                                     │
│   ┌─────────────┐          ┌─────────────┐                                  │
│   │ 0xFFF...F00 │ ◄─────── │ 0xFFF...F00 │  (RSP offset preserved)          │
│   │             │   COPY   │             │                                  │
│   │   ...       │ ──────►  │   ...       │                                  │
│   │             │          │             │                                  │
│   │ 0xFFF...000 │          │ 0xFFF...000 │  (stack base)                    │
│   └─────────────┘          └─────────────┘                                  │
│                                                                             │
│   Calculation:                                                              │
│   rsp_offset = parent_rsp - parent_stack_base                               │
│   child_rsp  = child_stack_base + rsp_offset                                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 Return Value Magic

The key to fork() is how both processes return different values:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    RETURN VALUE MECHANISM                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   In x86-64, syscall return value is in RAX register                        │
│                                                                             │
│   When fork() is called:                                                    │
│   ┌────────────────────────────────────────────────────────────┐            │
│   │ 1. Parent makes syscall (RAX = 57 = SYS_FORK)              │            │
│   │ 2. Kernel saves parent's RAX in parent's context.rax       │            │
│   │ 3. Kernel creates child with copied context                │            │
│   │ 4. Kernel SETS child's context.rax = 0                     │            │
│   │ 5. Kernel returns child's PID to parent                    │            │
│   └────────────────────────────────────────────────────────────┘            │
│                                                                             │
│   Result:                                                                   │
│   ┌──────────────┬──────────────────────────────────────┐                   │
│   │ Parent:      │ Child:                               │                   │
│   │ RAX = child  │ RAX = 0           (after context     │                   │
│   │   _PID       │                    switch)           │                   │
│   └──────────────┴──────────────────────────────────────┘                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Files Modified

### 4.1 Git Diff Summary

```
 kernel/include/proc.h    |   +5  (proc_fork declaration)
 kernel/include/syscall.h |   +3  (SYS_FORK = 57)
 kernel/proc/proc.c      | +164  (fork implementation)
 kernel/syscall/syscall.c|  +10  (sys_fork handler)
 kernel/kernel.c          |  +47  (test_fork function)
 ─────────────────────────────────────────
 5 files changed, 229 insertions(+)
```

### 4.2 Detailed Changes

| File | Change |
|------|--------|
| `kernel/include/syscall.h` | Added `#define SYS_FORK 57` |
| `kernel/include/proc.h` | Added `int proc_fork(void);` declaration |
| `kernel/proc/proc.c` | Added `copy_kernel_stack()`, `copy_user_memory()`, `proc_fork()` |
| `kernel/syscall/syscall.c` | Added `sys_fork()` handler, registered in table |
| `kernel/kernel.c` | Added `test_fork()` function and call in kernel_main |

---

## 5. Code Comparison: Phase 11 vs Phase 12

### Phase 11 - Basic Process Creation

```c
// Phase 11: Creating a completely NEW process
process_t* proc_create(void (*entry)(void), uint64_t stack_size) {
    // Parent doesn't exist - create from scratch
    process_t* proc = proc_table_alloc();
    proc->pid = proc_alloc_pid();
    proc->context.rip = (uint64_t)entry;  // NEW entry point
    // ...
    return proc;
}
```

### Phase 12 - Fork (Copy Process)

```c
// Phase 12: Fork - copy EXISTING process
int proc_fork(void) {
    process_t* parent = proc_current();
    
    // Create new process
    process_t* child = proc_table_alloc();
    child->pid = proc_alloc_pid();
    
    // COPY everything from parent
    child->context = parent->context;
    child->context.rax = 0;  // Child gets 0
    
    // Copy kernel stack
    copy_kernel_stack(parent, child);
    
    // Add to scheduler
    proc_add_to_ready(child);
    
    // Return child's PID to PARENT
    return child->pid;
}
```

---

## 6. Process Tree After Fork

```
BEFORE FORK:
┌─────────────────────────────────────┐
│  init (PID 1)                       │
│  └── bash (PID 2)                   │
│       └── our_program (PID 3)       │
└─────────────────────────────────────┘

AFTER FORK (our_program calls fork):
┌─────────────────────────────────────┐
│  init (PID 1)                       │
│  └── bash (PID 2)                   │
│       └── our_program (PID 3)       │
│              ├── child (PID 4)      │
│              └── (parent PID=3)     │
└─────────────────────────────────────┘

The child (PID 4) has:
- parent_pid = 3 (our_program)
- Same code, same data, same open files
- Different kernel stack
- Returns 0 to child, 4 to parent
```

---

## 7. Testing

### Test Function Flow

```c
void test_fork(void) {
    // Call fork()
    int32_t child_pid = proc_fork();
    
    if (child_pid > 0) {
        // PARENT BRANCH
        // child_pid contains child's PID
        terminal_writestring("Parent: created child PID X\n");
        timer_sleep(50);  // Let child run
    } 
    else if (child_pid == 0) {
        // CHILD BRANCH
        // This is the child process!
        terminal_writestring("Child: I'm running! (PID=Y)\n");
        proc_exit(42);
    } 
    else {
        // ERROR
        terminal_writestring("Fork failed!\n");
    }
}
```

### Expected Output

```
[TEST] Phase 12: fork() System Call
[FORK] Forking process PID 3
[FORK] Created child PID 4
  Parent: created child PID 4
  Child: I'm running! (PID=4)
[TEST] Phase 12 complete
```

---

## 8. Limitations & Future Enhancements

### Current (Simplified Fork)

- **User Memory**: Child shares parent's address space (no COW)
- **File Descriptors**: Not yet duplicated
- **No exec()**: Child runs same program as parent

### Future (Phase 15)

- **Copy on Write (COW)**: Share pages, copy on modification
- **Separate Address Spaces**: Full process isolation
- **exec() System Call**: Replace child's program

---

## 9. Why This Works

### The Magic of Context Copy

1. **Stack Copy**: When we copy the kernel stack, we copy the entire execution state including all local variables, return addresses, etc.

2. **Return Address**: Both parent and child have the same return address on their stacks (pointing to the instruction AFTER the syscall)

3. **rax = 0**: By setting child's context.rax = 0, when the child resumes (after context switch), it sees rax=0 as its return value

4. **Same Code Path**: Both processes execute the SAME code after fork() - the `if (pid > 0)` check determines which branch runs

---

## 10. References

- Unix Fork: https://en.wikipedia.org/wiki/Fork_(system_call)
- x86_64 System V ABI
- Linux Kernel Source: fork.c
- MakhOS Phase 9 (Process Management)
- MakhOS Phase 11 (Process Core Enhancement)
