# Phase 8: System Call Interface
## The Bridge Between User and Kernel Space

> "The system call is the fundamental boundary between user space and kernel space - the controlled gateway through which applications request privileged services from the operating system."

---

## Objective: Hardware-Accelerated Kernel Services

Every operating system requires a mechanism for user-mode programs to request services from the kernel:
- Write output to display
- Read input from devices  
- Sleep and wait for events
- Terminate execution

For x86_64 architecture, AMD introduced dedicated instructions: `syscall` and `sysret`

---

## Legacy vs Modern: Why int 0x80 is Obsolete

### The 32-bit Approach (DOES NOT WORK in x86_64)

```asm
; Legacy 32-bit Linux syscall method
mov eax, 4          ; sys_write number
mov ebx, 1          ; stdout fd
mov ecx, buffer     ; string pointer
mov edx, length     ; byte count
int 0x80            ; TRIGGER EXCEPTION IN LONG MODE
```

**Critical Issue:** x86_64 removed support for software-generated interrupts (`int` instruction) in 64-bit long mode. Any attempt to execute `int 0x80` results in an Invalid Opcode exception.

### The x86_64 Approach: syscall/sysret

```asm
; Modern x86_64 syscall method
mov rax, 1          ; sys_write number (RAX)
mov rdi, 1          ; stdout fd (RDI - 1st arg)
mov rsi, buffer     ; string pointer (RSI - 2nd arg)
mov rdx, length     ; byte count (RDX - 3rd arg)
syscall             ; Fast transition to kernel
```

**Advantages:**
- Faster execution: No IDT lookup required
- Native x86_64 support: Designed for long mode
- Hardware-enforced security: MSR-based configuration

---

## System Call Execution Flow

### User Mode to Kernel Mode Transition

```
    USER MODE (Ring 3)                    KERNEL MODE (Ring 0)
    
    +------------------+                  +------------------+
    |  User Program    |                  |                  |
    |                  |                  |                  |
    |  mov rax, SYSNO  |                  |                  |
    |  mov rdi, arg1   |                  |                  |
    |  mov rsi, arg2   |                  |                  |
    |  mov rdx, arg3   |                  |                  |
    |                  |                  |                  |
    |  syscall         | ---------------+ |                  |
    |       |          |    HARDWARE      |                  |
    |       |          |    DOES THIS:    |                  |
    |       |          |    1. RCX = RIP  |                  |
    |       |          |    2. R11 = RFLAGS|                 |
    |       |          |    3. RIP = LSTAR|                  |
    |       |          |    4. Ring 0 mode|                  |
    |       |          |                  |                  |
    |       v          |                  |  syscall_entry() |
    |    PAUSED        |                  |       |          |
    |                  |                  |       v          |
    |                  |                  |  push registers  |
    |                  |                  |       |          |
    |                  |                  |       v          |
    |                  |                  |  call syscall_handler
    |                  |                  |       |          |
    |                  |                  |       v          |
    |                  |                  |  dispatch table  |
    |                  |                  |       |          |
    |                  |                  |       v          |
    |                  |                  |  sys_write()     |
    |                  |                  |  (or other call) |
    |                  |                  |       |          |
    |       ^          |                  |       v          |
    |       |          |                  |  pop registers   |
    |       |          |                  |       |          |
    |       |          |    HARDWARE      |       v          |
    |       |          |    DOES THIS:    |  sysretq         |
    |    RESUMES       | <---------------+|  1. RIP = RCX    |
    |                  |    1. RIP = RCX  |  2. RFLAGS = R11 |
    |                  |    2. RFLAGS=R11 |  3. Ring 3 mode  |
    |                  |    3. Ring 3 mode|                  |
    +------------------+                  +------------------+
```

---

## Model-Specific Registers (MSRs)

The x86_64 syscall mechanism is configured through four MSRs:

```
MSR Register Map:
=================

IA32_EFER (0xC0000080)
+------------------+
| Bit 0: SCE       |  <-- Syscall Enable (must set to 1)
| Bits 1-63: ...   |
+------------------+

IA32_STAR (0xC0000081)  
+----------------------------------+
| Bits 63:48 = Kernel CS (0x08)    |
| Bits 47:32 = User CS   (0x1B)    |
| Bits 31:0  = Reserved            |
+----------------------------------+

IA32_LSTAR (0xC0000082)
+------------------------------+
| 64-bit address of            |
| syscall_entry function       |
+------------------------------+

IA32_FMASK (0xC0000084)
+------------------------------+
| RFLAGS mask value            |
| (0x200 = clear interrupt flag)|
+------------------------------+
```

### CS/SS Selector Calculation

```
IA32_STAR layout for selectors:

    63           48 47           32 31            0
    +---------------+---------------+---------------+
    |   Kernel CS   |    User CS    |   Reserved    |
    |    (0x08)     |    (0x1B)     |               |
    +---------------+---------------+---------------+

Derived selectors:
- Kernel CS = 0x08
- Kernel SS = 0x10 (CS + 8)
- User CS   = 0x1B (0x18 | 3 [RPL])
- User SS   = 0x23 (CS + 8)
```

---

## Implementation Architecture

### 1. System Call Numbers (syscall.h)

```c
/* System Call Number Definitions */
#define SYS_EXIT        0   /* Process termination */
#define SYS_WRITE       1   /* Output to file descriptor */
#define SYS_READ        2   /* Input from file descriptor */
#define SYS_OPEN        3   /* Open file */
#define SYS_CLOSE       4   /* Close file descriptor */
#define SYS_GETPID      5   /* Get process ID */
#define SYS_SLEEP       6   /* Sleep for duration */
#define SYS_GETTICKS    7   /* Get timer tick count */

#define MAX_SYSCALLS    64  /* Maximum supported syscalls */
```

### 2. Assembly Entry Point (syscall_asm.asm)

```asm
; syscall_entry - CPU jumps here when syscall instruction executes
; 
; ON ENTRY:
;   RAX = syscall number
;   RDI = arg1, RSI = arg2, RDX = arg3
;   RCX = user RIP (return address)
;   R11 = user RFLAGS
;   CS = kernel code selector, SS = kernel stack selector

syscall_entry:
    ; Save all general purpose registers
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx        ; Return address
    push rbx
    
    ; Arguments are already in correct registers
    ; per System V AMD64 ABI:
    ; RDI = arg0, RSI = arg1, RDX = arg2, R10 = arg3, R8 = arg4, R9 = arg5
    
    ; Call C handler
    call syscall_handler
    
    ; Restore registers
    pop rbx
    add rsp, 8      ; Skip RCX (sysretq uses it for RIP)
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    
    ; Return to user space
    ; CPU restores RIP from RCX, RFLAGS from R11
    sysretq
```

### 3. MSR Initialization (syscall.c)

```c
void syscall_init(void) {
    uint64_t efer;
    
    /* Step 1: Enable syscall in EFER MSR */
    efer = rdmsr(IA32_EFER);
    efer |= EFER_SCE;           /* Set Syscall Enable bit */
    wrmsr(IA32_EFER, efer);
    
    /* Step 2: Configure segment selectors in STAR MSR */
    /* Bits 63:48 = Kernel CS (0x08), Bits 47:32 = User CS (0x18 | 3) */
    wrmsr(IA32_STAR, (0x08ULL << 48) | (0x1BULL << 32));
    
    /* Step 3: Set syscall entry point in LSTAR MSR */
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    
    /* Step 4: Configure RFLAGS mask in FMASK MSR */
    /* 0x200 = Interrupt Flag (IF) - cleared on syscall entry */
    wrmsr(IA32_FMASK, 0x200);
}
```

### 4. Handler Dispatcher (syscall.c)

```c
/* Syscall function prototype: return value and up to 6 arguments */
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, 
                                  uint64_t, uint64_t, uint64_t);

/* Dispatch table */
static syscall_fn_t syscall_table[MAX_SYSCALLS];

uint64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    /* Bounds check */
    if (num >= MAX_SYSCALLS) {
        return -1;  /* Invalid syscall number */
    }
    
    /* Check if syscall is implemented */
    if (!syscall_table[num]) {
        return -1;  /* Syscall not implemented */
    }
    
    /* Dispatch to handler */
    return syscall_table[num](a1, a2, a3, a4, a5, 0);
}
```

### 5. Syscall Registration (syscall.c)

```c
void syscall_register(uint64_t num, syscall_fn_t fn) {
    if (num < MAX_SYSCALLS) {
        syscall_table[num] = fn;
    }
}

void syscall_init(void) {
    /* ... MSR setup ... */
    
    /* Register system calls */
    syscall_register(SYS_EXIT,     sys_exit);
    syscall_register(SYS_WRITE,    sys_write);
    syscall_register(SYS_GETPID,   sys_getpid);
    syscall_register(SYS_SLEEP,    sys_sleep);
    syscall_register(SYS_GETTICKS, sys_getticks);
}
```

### 6. System Call Implementations

```c
/* Process termination */
static uint64_t sys_exit(uint64_t code, uint64_t _, uint64_t __,
                         uint64_t ___, uint64_t ____, uint64_t _____) {
    terminal_writestring("[SYSCALL] exit(");
    terminal_write_hex(code);
    terminal_writestring(") - halting system\n");
    
    while (1) {
        __asm__ __volatile__ ("cli; hlt");
    }
    return 0;
}

/* Write to file descriptor */
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t _, uint64_t __, uint64_t ___) {
    if (fd == 1) {  /* stdout */
        const char* str = (const char*)buf;
        for (uint64_t i = 0; i < count && str[i]; i++) {
            terminal_putchar(str[i]);
        }
        return count;
    }
    return -1;
}

/* Get process ID (placeholder) */
static uint64_t sys_getpid(uint64_t _, uint64_t __, uint64_t ___,
                           uint64_t ____, uint64_t _____, uint64_t ______) {
    return 1;  /* Single process system */
}

/* Sleep for duration */
static uint64_t sys_sleep(uint64_t ms, uint64_t _, uint64_t __,
                          uint64_t ___, uint64_t ____, uint64_t _____) {
    uint32_t start = get_ticks();
    uint32_t ticks_needed = ms / TICK_INTERVAL_MS;
    
    while ((get_ticks() - start) < ticks_needed) {
        __asm__ __volatile__ ("hlt");
    }
    return 0;
}

/* Get timer ticks */
static uint64_t sys_getticks(uint64_t _, uint64_t __, uint64_t ___,
                             uint64_t ____, uint64_t _____, uint64_t ______) {
    return get_ticks();
}
```

---

## Kernel Integration

### Initialization Sequence

```
kernel_main()
    |
    +-- gdt_init()          [Phase 7]
    |
    +-- idt_init()          [Phase 4]
    |
    +-- timer_init()        [Phase 5]
    |
    +-- syscall_init()      [Phase 8] <-- NEW
    |   |
    |   +-- Configure EFER.SCE
    |   +-- Configure STAR MSR
    |   +-- Set LSTAR to syscall_entry
    |   +-- Configure FMASK
    |   +-- Register syscalls
    |
    +-- test_syscalls()     [Phase 8] <-- NEW
```

---

## Testing and Verification

### Test Execution Output

```
[TEST] Testing System Calls...
  sys_getpid() = 1 (expected: 1)
  [PASS] getpid test
  
  Writing to stdout via syscall...
  Hello from syscall!
  sys_write returned 23 bytes
  [PASS] write test
  
  sys_getticks() = 14
  [PASS] getticks test
  
  Sleeping for 1 second...
  [TIMER] Tick: 1 seconds
  After sleep: ticks = 121 (delta: 107)
  [PASS] sleep test
  
  Testing invalid syscall (number 99)...
  Invalid syscall returned -1 (expected: -1)
  [PASS] invalid syscall handling
  
[TEST] All system call tests passed
```

---

## Performance Analysis

### Mechanism Comparison

```
+----------------+-------------+-------------------+----------------------------+
| Mechanism      | Cycles      | Memory Accesses   | Notes                      |
+----------------+-------------+-------------------+----------------------------+
| int 0x80       | ~100+       | IDT lookup,       | Not supported in x86_64    |
|                |             | stack switches    | long mode                  |
+----------------+-------------+-------------------+----------------------------+
| syscall/sysret | ~30-40      | MSR read (cached) | Native x86_64 method       |
+----------------+-------------+-------------------+----------------------------+
| sysenter/exit  | ~40-50      | MSR read          | Intel 32-bit, deprecated   |
+----------------+-------------+-------------------+----------------------------+

Performance advantage: syscall is approximately 3x faster than legacy int 0x80
```

---

## Future Work: User Mode Foundation

This syscall implementation enables:

1. **User Processes**: Ring 3 execution with hardware protection
2. **Process Isolation**: Separate address spaces per process
3. **Context Switching**: Save/restore process state
4. **Standard Library**: C library (printf, malloc, etc.) built on syscalls

---

## Files Created and Modified

```
Created:
--------
kernel/include/syscall.h        40 lines    Syscall numbers and API
kernel/arch/syscall_asm.asm     99 lines    Assembly entry stub
kernel/syscall/syscall.c       160 lines    MSR setup and handlers

Modified:
---------
kernel/arch/idt.c                5 lines    Removed int 0x80 gate
kernel/kernel.c                 66 lines    Added init and tests
Makefile                         2 lines    Build integration

Total: ~372 lines of syscall implementation
```

---

## Technical References

- AMD64 Architecture Programmer's Manual, Volume 2: System Programming
- Intel SDM Volume 2: Instruction Set Reference (syscall/sysret)
- System V AMD64 ABI: Calling conventions
