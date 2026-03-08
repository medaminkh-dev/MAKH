# MakhOS Phase 11: Process Core Enhancement

**Version:** 0.0.3  
**Date:** 2026-03-07  
**Status:** COMPLETE - Working

---

## 1. Overview

Phase 11 builds upon the basic process management from **Phase 9** by adding advanced process management features:

- Process Table with MAX_PROCESSES (256 processes)
- PID Management (bitmap-based allocation up to 32768 PIDs)
- Process Tree (parent-child relationships)
- Process Statistics (CPU time, creation time, priority)
- Process Info System Calls

This phase transforms the simple process model into a full-featured process management system similar to traditional Unix-like OSes.

---

## 2. What's Different from Phase 9?

### Phase 9 vs Phase 11 Comparison

| Feature | Phase 9 (v0.0.2) | Phase 11 (v0.0.3) |
|---------|------------------|-------------------|
| **Max Processes** | Unlimited (heap-based) | 256 (process table) |
| **PID Allocation** | Simple counter (`next_pid++`) | Bitmap-based (32KB bitmap) |
| **Process Lookup** | Linear search in linked list | O(1) lookup via process table |
| **Parent-Child** | Not supported | Full tree structure |
| **Priority** | Fixed (time-slice only) | Variable (0-255) |
| **Process Statistics** | Not tracked | CPU time, creation time |
| **Exit Codes** | Not stored | Stored for parent collection |
| **System Calls** | Basic (getpid) | Extended (getppid, getpriority, setpriority) |

---

## 3. Architecture

### 3.1 Process Table

```
┌─────────────────────────────────────────────────────────────────┐
│                    Process Table                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   process_table[MAX_PROCESSES]                                   │
│   ┌────────┬────────┬────────┬────────┬────────┬────────┐        │
│   │ Slot 0 │ Slot 1 │ Slot 2 │ Slot 3 │  ...   │Slot 255│        │
│   │  NULL  │  PID 1 │  PID 2 │  PID 4 │   -    │  NULL  │        │
│   └────────┴────────┴────────┴────────┴────────┴────────┘        │
│                                                                  │
│   process_count = 3  (active processes)                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 PID Bitmap

```
┌─────────────────────────────────────────────────────────────────┐
│                    PID Bitmap Allocation                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│   pid_bitmap[4096 bytes] = 32768 bits                           │
│                                                                  │
│   Byte 0:                                                        │
│   ┌──┬──┬──┬──┬──┬──┬──┬──┐                                    │
│   │b0│b1│b2│b3│b4│b5│b6│b7│  (each bit = 1 PID)             │
│   └──┴──┴──┴──┴──┴──┴──┴──┘                                    │
│    │                                                      │
│    └── bit 0 = 1 (PID 0 reserved for idle)                   │
│                                                                  │
│   Allocation: Find first 0 bit, set to 1                        │
│   Deallocation: Find bit, set to 0                             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.3 Process Tree Structure

```
┌─────────────────────────────────────────────────────────────────┐
│                    Process Tree (Parent-Child)                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│                         init (PID 1)                            │
│                        ┌───────────┐                            │
│                        │ parent=0  │                            │
│                        │ children──┼──► ┌─────┐ ┌─────┐        │
│                        └───────────┘  │PID 2│ │PID 3│        │
│                                         └─────┘ └─────┘        │
│                                              │                  │
│                                              ▼                  │
│                                         ┌─────┐                │
│                                         │PID 4│                │
│                                         │parent=2             │
│                                         └─────┘                │
│                                                                  │
│   When parent dies:                                              │
│   - Children reparented to init (PID 1)                         │
│   - Called "orphan adoption"                                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.4 Enhanced Process Control Block (PCB)

```
┌─────────────────────────────────────────────────────────────────┐
│                  process_t (Enhanced PCB)                        │
├─────────────────────────────────────────────────────────────────┤
│  BASIC PROCESS INFO                                              │
│  ┌─────────────┬────────────────────────────────────────────┐   │
│  │ pid         │ Unique Process ID (1-32767)                │   │
│  │ parent_pid  │ Parent's PID (0 if orphan/init)           │   │
│  │ state       │ EMBRYO/READY/RUNNING/BLOCKED/ZOMBIE        │   │
│  │ priority    │ Scheduling priority (0-255, higher=more)   │   │
│  │ name[32]    │ Process name (for debugging)              │   │
│  │ exit_code   │ Exit code (stored for parent collection)  │   │
│  └─────────────┴────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  PROCESS TREE                                                   │
│  ┌─────────────┬────────────────────────────────────────────┐   │
│  │ child_count │ Number of child processes                 │   │
│  │ children_head│ Pointer to first child (linked list)      │   │
│  │ children_tail│ Pointer to last child                     │   │
│  │ next        │ Next sibling in parent's list             │   │
│  └─────────────┴────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  STATISTICS                                                     │
│  ┌─────────────┬────────────────────────────────────────────┐   │
│  │ creation_time│ Timer ticks when process was created      │   │
│  │ cpu_time_used│ Total CPU time consumed (ticks)           │   │
│  └─────────────┴────────────────────────────────────────────┘   │
├─────────────────────────────────────────────────────────────────┤
│  CONTEXT (from Phase 9)                                         │
│  ┌─────────────┬────────────────────────────────────────────┐   │
│  │ context     │ Saved registers (rip, rsp, rflags, etc.)   │   │
│  │ kernel_stack│ Base address of kernel stack               │   │
│  │ kernel_stack_size│ Size of kernel stack                 │   │
│  └─────────────┴────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. Data Structures

### 4.1 Constants Added

| Constant | Value | Description |
|----------|-------|-------------|
| MAX_PROCESSES | 256 | Maximum concurrent processes |
| PID_MAX | 32768 | Maximum PID value (16-bit) |

### 4.2 New Process States (Unchanged from Phase 9)

| State | Value | Description |
|-------|-------|-------------|
| PROC_EMBRYO | 0 | Process being created |
| PROC_READY | 1 | In ready queue, waiting for CPU |
| PROC_RUNNING | 2 | Currently executing on CPU |
| PROC_BLOCKED | 3 | Waiting for I/O or event |
| PROC_ZOMBIE | 4 | Terminated, waiting for parent |

### 4.3 New System Calls Added

| Syscall | Number | Description |
|---------|--------|-------------|
| SYS_GETPID | 5 | Get current process PID (already existed) |
| SYS_GETPPID | 8 | Get parent PID |
| SYS_GETPRIORITY | 9 | Get process priority |
| SYS_SETPRIORITY | 10 | Set process priority |

---

## 5. Files Modified

### 5.1 Summary of Changes

| File | Changes |
|------|---------|
| `kernel/include/proc.h` | Added MAX_PROCESSES, PID_MAX, new PCB fields, function declarations |
| `kernel/proc/proc.c` | Added process table, PID bitmap, tree management, modified proc_create/exit |
| `kernel/include/syscall.h` | Added SYS_GETPPID, SYS_GETPRIORITY, SYS_SETPRIORITY |
| `kernel/syscall/syscall.c` | Added syscall implementations |
| `kernel/kernel.c` | Added test_phase11() function |

### 5.2 Git Diff Summary

```
 kernel/include/proc.h          |  31 +++
 kernel/include/syscall.h       |   3 +
 kernel/kernel.c                |  51 +++++++
 kernel/proc/proc.c             | 321 ++++++++++++++++++++++++++++++++
 kernel/syscall/syscall.c       |  35 +++++
 6 files changed, 567 insertions(+), 160 deletions(-)
```

---

## 6. API Reference

### 6.1 New Process Functions

```c
// Process Table Management
process_t* proc_table_alloc(void);    // Allocate slot in process table
void proc_table_free(process_t* proc); // Free slot in process table

// PID Management
int32_t proc_alloc_pid(void);         // Allocate unique PID
void proc_free_pid(int32_t pid);       // Release PID back to bitmap

// Process Lookup
process_t* proc_find(int32_t pid);    // Find process by PID
```

### 6.2 New System Calls

```c
// Get parent PID
// Returns: parent_pid of current process
uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Get process priority
// a1: pid of target process
// Returns: priority (0-255) or -1 on error
uint64_t sys_getpriority(uint64_t pid, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

// Set process priority
// a1: pid of target process
// a2: new priority (0-255)
// Returns: 0 on success, -1 on error
uint64_t sys_setpriority(uint64_t pid, uint64_t priority, uint64_t a3, uint64_t a4, uint64_t a5);
```

---

## 7. Implementation Details

### 7.1 PID Allocation Algorithm

```
proc_alloc_pid():
    FOR i FROM 1 TO PID_MAX-1:
        byte_idx = i / 8
        bit_idx = i % 8
        IF (pid_bitmap[byte_idx] & (1 << bit_idx)) == 0:
            pid_bitmap[byte_idx] |= (1 << bit_idx)
            RETURN i
    RETURN -1  // No free PIDs
```

### 7.2 Process Creation Flow (Enhanced)

```
proc_create(entry, stack_size):
    1. proc_table_alloc() - Get slot from process table
    2. proc_alloc_pid()  - Get unique PID from bitmap
    3. Initialize new fields:
       - parent_pid = current_process->pid
       - priority = 128 (default middle)
       - creation_time = timer_get_ticks()
       - cpu_time_used = 0
       - child_count = 0
       - children_head/tail = NULL
       - name = "kernel_thread"
    4. Allocate kernel stack
    5. Set up context (from Phase 9)
    6. proc_add_to_ready(proc)
    7. proc_add_child(parent, proc) - Add to parent's children list
    8. RETURN proc
```

### 7.3 Process Exit Flow (Enhanced)

```
proc_exit(code):
    1. Store exit_code = code
    2. Set state = PROC_ZOMBIE
    3. proc_reparent_orphans(self) - Children adopted by init
    4. proc_remove_child(self) - Remove from parent's list
    5. proc_free_pid(pid) - Release PID back to bitmap
    6. proc_yield() - Switch to next process
```

### 7.4 Orphan Reparenting

```
proc_reparent_orphans(dead_parent):
    init = proc_find(1)
    child = dead_parent->children_head
    
    WHILE child != NULL:
        next = child->next
        child->parent_pid = 1
        
        // Add to init's children list
        IF init->children_tail != NULL:
            init->children_tail->next = child
            init->children_tail = child
        ELSE:
            init->children_head = child
            init->children_tail = child
        
        init->child_count++
        child = next
    
    // Clear dead parent's list
    dead_parent->children_head = NULL
    dead_parent->children_tail = NULL
    dead_parent->child_count = 0
```

---

## 8. Testing

### 8.1 Test Function

```c
void test_phase11(void) {
    char buf[32];
    
    terminal_writestring("\n[TEST] Phase 11: Process Core Enhancement\n");
    
    // Test PID allocation
    int32_t pid1 = proc_alloc_pid();
    int32_t pid2 = proc_alloc_pid();
    terminal_writestring("  PID1: ");
    uint64_to_string(pid1, buf);
    terminal_writestring(buf);
    terminal_writestring(", PID2: ");
    uint64_to_string(pid2, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // Test process creation
    process_t* p1 = proc_create(NULL, 4096);
    process_t* p2 = proc_create(NULL, 4096);
    
    if (p1 && p2) {
        terminal_writestring("  Created processes: PID ");
        uint64_to_string(p1->pid, buf);
        terminal_writestring(buf);
        terminal_writestring(" and PID ");
        uint64_to_string(p2->pid, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
        
        // Test proc_find
        process_t* found = proc_find(p1->pid);
        if (found == p1) {
            terminal_writestring("  ✅ proc_find works\n");
        }
        
        // Test parent-child relationship
        if (found->parent_pid == proc_current()->pid) {
            terminal_writestring("  ✅ Parent-child link works\n");
        }
    }
    
    terminal_writestring("[TEST] Phase 11 complete\n");
}
```

### 8.2 Expected Output

```
[TEST] Phase 11: Process Core Enhancement
  PID1: 1, PID2: 2
  Created processes: PID 3 and PID 4
  ✅ proc_find works
  ✅ Parent-child link works
[TEST] Phase 11 complete
```

### 8.3 Verification

- ✅ Kernel compiles successfully (make clean && make)
- ✅ Symbol check shows all new functions:
  - proc_table_alloc
  - proc_alloc_pid
  - proc_table_free
  - proc_find

---

## 9. Future Enhancements (Phase 12+)

- [ ] Process wait() / waitpid() system calls
- [ ] Fork/exec system calls
- [ ] Sleep/wakeup mechanism
- [ ] Process signals
- [ ] Virtual memory per process
- [ ] File descriptors per process
- [ ] Process accounting

---

## 10. References

- x86_64 System V ABI
- Intel® 64 and IA-32 Architectures Software Developer's Manual
- OSDEV Wiki: Process Management
- POSIX Process Model
- MakhOS Previous Phases (1-10)
