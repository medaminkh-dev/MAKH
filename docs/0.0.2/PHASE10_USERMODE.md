# MakhOS Phase 10: User Mode Transition & Execution

**Version:** 0.0.2 (Final)  
**Date:** 2026-03-06  
**Status:** COMPLETE - v0.0.2 Release

---

## 1. Overview

Phase 10 implements **user mode execution** and **privilege level transitions** for MakhOS. This phase introduces the critical mechanisms for running unprivileged user programs ring 3 while maintaining kernel integrity and security.

### Key Features Implemented:

- **Ring Transition Architecture**: Transition from Ring 0 (kernel) to Ring 3 (user) using `iretq`
- **User Mode Entry**: `enter_usermode()` function sets up execution environment
- **Memory Isolation**: User code/stack mapped with `PAGE_USER` flag
- **Syscall Interface**: User programs invoke kernel services via `syscall` instruction
- **Protected Execution**: Segment registers properly configured for privilege levels
- **User Program Support**: Sample user program with syscalls and standalone execution

### Security Principles:

- User programs execute with restricted privileges (Ring 3)
- User mode code cannot access kernel memory (enforced by page tables)
- All kernel access through controlled syscall interface
- Interrupts properly return to correct privilege level

---

## 2. Architecture

### 2.1 Privilege Level Hierarchy

```
┌──────────────────────────────────────────────────────────────────┐
│                    x86-64 Privilege Levels                       │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│                    RING 0 (Kernel Mode)                          │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │                                                         │     │
│  │  - Full processor privilege                             │     │
│  │  - Access to all memory (kernel + user)                │    │
│  │  - Can load segment registers and control regs         │    │
│  │  - Can execute privileged instructions                 │    │
│  │    (iretq, sysret, mov cr3, etc.)                      │    │
│  │                                                         │    │
│  │  Kernel Code Segment: 0x08 (RPL=0)                     │    │
│  │  Kernel Data Segment: 0x10 (RPL=0)                     │    │
│  │                                                         │    │
│  └─────────────────────────────────────────────────────────┘    │
│                           ▲                                      │
│                           │                                      │
│        ┌──────────────────┴──────────────────┐                  │
│        │    SYSCALL INTERFACE                │                  │
│        │    (Controlled Entry Point)          │                  │
│        │                                      │                  │
│        └──────────────────┬──────────────────┘                  │
│                           │                                      │
│                           ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                                                         │    │
│  │  ┏━━━━━━━━━━━━━━━━ RING 3 (User Mode) ━━━━━━━━━━━━━━━┓│    │
│  │  ┃                                                      ┃│    │
│  │  ┃  - Limited processor privilege                       ┃│    │
│  │  ┃  - Access to user memory only (pages with PAGE_USER)┃│    │
│  │  ┃  - Cannot access kernel memory (protection fault)   ┃│    │
│  │  ┃  - Cannot load segment registers                    ┃│    │
│  │  ┃  - Cannot execute privileged instructions           ┃│    │
│  │  ┃  - Can use syscall for kernel services              ┃│    │
│  │  ┃  - Can install interrupt handlers (with limits)     ┃│    │
│  │  ┃                                                      ┃│    │
│  │  ┃  User Code Segment: 0x1B (0x18 | RPL=3)             ┃│    │
│  │  ┃  User Data Segment: 0x23 (0x20 | RPL=3)             ┃│    │
│  │  ┃                                                      ┃│    │
│  │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛│    │
│  │                                                         │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 Ring Transition Mechanism (iretq Path)

```
┌──────────────────────────────────────────────────────────────────┐
│         Kernel Mode (Ring 0) → User Mode (Ring 3)                │
│                    via iretq instruction                         │
└──────────────────────────────────────────────────────────────────┘

Step 1: Build iretq Frame on Kernel Stack
         (Lower addresses ← Stack grows ← Higher addresses)

         Stack Before Build:
              ┌─────────────────┐  rsp = low addr
              │                 │
              │  Kernel Stack   │
              │  (unused area)  │
              │                 │
              └─────────────────┘


         After "sub rsp, 40":
              ┌─────────────────┐
              │  [rsp+32] SS    │  0x23 (User DS | Ring 3)
              │  [rsp+24] RSP   │  User stack pointer
              │  [rsp+16] RFLAGS│  Flags + IF=1 (interrupts enabled)
              │  [rsp+8]  CS    │  0x1B (User CS | Ring 3)
              │  [rsp+0]  RIP   │  Entry point (user program address)
              └─────────────────┘  rsp


Step 2: Execute iretq Instruction
         
         iretq: Interrupt Return
         
         CPU Actions (in order):
         ┌────────────────────────────────────────┐
         │  1. Pop RIP from [rsp+0]                │
         │     rip ← entry point                  │
         │     rsp ← rsp + 8                      │
         │                                        │
         │  2. Pop CS from [rsp+0] (now rsp+8)    │
         │     cs ← 0x1B (Ring 3)                 │
         │                                        │
         │     CPU: Detects CS.RPL = 3           │
         │           Ring switch to Ring 3!      │
         │                                        │
         │  3. Pop RFLAGS from [rsp+0]            │
         │     rflags ← user flags                │
         │     rsp ← rsp + 8                      │
         │                                        │
         │  4. Pop RSP from [rsp+0]               │
         │     rsp ← user stack pointer           │
         │                                        │
         │  5. Pop SS from [rsp+0]                │
         │     ss ← 0x23 (Ring 3)                 │
         │                                        │
         └────────────────────────────────────────┘


Step 3: Execution Continues in User Mode

         ┌──────────────────────────────────────────┐
         │  Now executing at Ring 3:                │
         │  - CS = 0x1B (user code segment)         │
         │  - SS = 0x23 (user data segment)         │
         │  - RIP = entry point (user program)      │
         │  - RSP = user stack top                  │
         │  - CPL (current privilege level) = 3    │
         │                                          │
         │  User program executes normal code:      │
         │            mov rax, 1        # syscall   │
         │            mov rdx, 5        # length    │
         │            syscall           # invoke    │
         │  (or any regular user code)              │
         │                                          │
         └──────────────────────────────────────────┘


Step 4: Return to Ring 0 via Syscall
         
         When user code executes "syscall":
         
         syscall: System Call
         
         CPU Actions:
         ┌────────────────────────────────────────┐
         │  1. Save user RIP → RCX                 │
         │     Save user RFLAGS → R11              │
         │     (User state preserved)              │
         │                                        │
         │  2. Load CS from STAR MSR[63:48]        │
         │     cs ← 0x08 (kernel code)             │
         │                                        │
         │  3. Load SS from STAR MSR[63:48] + 8    │
         │     ss ← 0x10 (kernel data)             │
         │                                        │
         │  4. Load RIP from LSTAR MSR              │
         │     rip ← syscall_entry function        │
         │                                        │
         │  5. CPU switches to Ring 0               │
         │     CPL = 0 (kernel mode)               │
         │                                        │
         └────────────────────────────────────────┘

         CPU saves RCX and R11 (NOT on stack!)
         This is critical: R11 contains RFLAGS
         which includes the previous privilege level
```

### 2.3 Memory Layout: User Program Setup

```
┌──────────────────────────────────────────────────────────────────┐
│                 Virtual Address Layout                           │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  0xFFFF800000000000  ┌─────────────────────────────────────┐   │
│                      │      Kernel Address Space          │   │
│                      │  (Ring 0 only)                     │   │
│                      │  - Page tables                     │   │
│                      │  - Kernel code                     │   │
│                      │  - Kernel data                     │   │
│                      │  - Kernel heap                     │   │
│                      │  PAGE_USER = 0 (no access)        │   │
│                      └─────────────────────────────────────┘   │
│                               ▼                                 │
│  (Canonical hole)   ┌─────────────────────────────────────┐   │
│                      │    (Hole in address space)        │   │
│                      │    Not mapped                      │   │
│                      └─────────────────────────────────────┘   │
│                               ▼                                 │
│  0x00007FFFFFFFFFFF  ┌─────────────────────────────────────┐   │
│                      │                                    │   │
│                      │      User Address Space           │   │
│                      │      (Ring 3 accessible)          │   │
│                      │                                    │   │
│  User Stack Region   │    ┌──────────────────────────┐   │   │
│  0x70008000          │    │   User Stack (High)      │   │   │
│  (Stack top)         │    │   Page: 0x70004000       │   │   │
│                      │    │   Page: 0x70000000       │   │   │
│  ─────────────────   │    │   Size: 2 × 4KB = 8KB    │   │   │
│  User Code Region    │    │   PAGE_PRESENT | USER    │   │   │
│  0x60001000          │    │                          │   │   │
│  (Code length)       │    │   PAGE_USER = 1 ✓        │   │   │
│  0x60000000          │    ├──────────────────────────┤   │   │
│  (Code starts)       │    │   User Code              │   │   │
│                      │    │   Page: 0x60000000       │   │   │
│                      │    │   Size: 1 × 4KB = 4KB    │   │   │
│                      │    │   PAGE_PRESENT | USER    │   │   │
│                      │    │   PAGE_USER = 1 ✓        │   │   │
│                      │    └──────────────────────────┘   │   │
│                      │                                    │   │
│                      │    (Other user allocations)       │   │
│                      │                                    │   │
│  0x00000000          └─────────────────────────────────────┘   │
│                                                                  │
├──────────────────────────────────────────────────────────────────┤
│ Key: PAGE_USER flag in PTE distinguishes User vs Kernel pages   │
│      User mode code (Ring 3) cannot access pages without        │
│      PAGE_USER = 1 (generates #PF protection fault)             │
└──────────────────────────────────────────────────────────────────┘
```

### 2.4 Syscall Argument Passing Convention

```
┌──────────────────────────────────────────────────────────────────┐
│       System Call Argument Convention (Linux x64 Compatible)     │
├──────────────────────────────────────────────────────────────────┤
│                                                                  │
│  User Mode (Ring 3) → Syscall Invocation                        │
│                                                                  │
│  Registers IN (before syscall instruction):                     │
│  ┌──────────┬───────────┬─────────────────────────────────────┐│
│  │ Register │ Content   │ Description                         ││
│  ├──────────┼───────────┼─────────────────────────────────────┤│
│  │ RAX      │ SYS_ID    │ System call number (1-6)            ││
│  │ RDI      │ arg1      │ First argument                      ││
│  │ RSI      │ arg2      │ Second argument                     ││
│  │ RDX      │ arg3      │ Third argument                      ││
│  │ R10      │ arg4      │ Fourth argument (NOT RCX!)          ││
│  │ R8       │ arg5      │ Fifth argument                      ││
│  │ R9       │ arg6      │ Sixth argument                      ││
│  └──────────┴───────────┴─────────────────────────────────────┘│
│                                                                  │
│  NOTE: RCX is NOT used for arg4 because:                        │
│        syscall instruction saves user RIP → RCX                 │
│        Syscall MSR controls can't override this!                │
│                                                                  │
│  Kernel Mode (Ring 0) → Syscall Handler                         │
│                                                                  │
│  Input State (in syscall_entry):                                │
│  ┌──────────┬───────────┬─────────────────────────────────────┐│
│  │ RCX      │ User RIP  │ Saved by CPU (do not clobber)      ││
│  │ R11      │ User RFLAGS│ Saved by CPU (used for sysret)    ││
│  │ RAX      │ SYS_ID    │ Still has syscall number           ││
│  │ RDI-R9   │ Arguments │ All arguments preserved            ││
│  └──────────┴───────────┴─────────────────────────────────────┘│
│                                                                  │
│  Return to User Mode (via sysret):                              │
│  ┌──────────┬───────────┬─────────────────────────────────────┐│
│  │ RAX      │ Result    │ Return value (or error code)        ││
│  │ RCX      │ User RIP  │ Restored from saved value           ││
│  │ R11      │ User RFLAGS│ Restored from saved value          ││
│  └──────────┴───────────┴─────────────────────────────────────┘│
│                                                                  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.5 System Call Table (Ring 0 → User Services Interface)

```
┌────────────────────────────────────────────────────────┐
│         Syscall Dispatch Table                        │
├────────────────────────────────────────────────────────┤
│                                                        │
│  User issues: syscall                                 │
│       │                                                │
│       ▼ CPU does:                                      │
│  1. Save RIP → RCX, RFLAGS → R11                      │
│  2. Load RIP from LSTAR (syscall_entry)               │
│  3. Load CS,SS from STAR                              │
│  4. Switch to Ring 0                                  │
│       │                                                │
│       ▼ Kernel:                                        │
│  syscall_entry (asm):                                 │
│    - Push RCX, R11 on stack                           │
│    - Save registers                                   │
│    - Call syscall_handler (C function)                │
│       │                                                │
│       ├─→ RAX = 1 (SYS_WRITE)?  → sys_write()          │
│       ├─→ RAX = 60 (SYS_EXIT)?  → sys_exit()           │
│       ├─→ RAX = 0 (SYS_READ)?   → sys_read()           │
│       ├─→ RAX = 95 (SYS_GETTICKS)? → sys_getticks()   │
│       ├─→ RAX = 57 (SYS_SLEEP)? → sys_sleep()         │
│       └─→ RAX = (invalid)?      → return -1           │
│                                                        │
│  sys_write(fd, buf, len):                             │
│    - 1: STDOUT → terminal_writestring()               │
│    - Returns: bytes written                           │
│                                                        │
│  sys_exit(code):                                      │
│    - Terminates user program                          │
│    - Returns to kernel shell                          │
│    - Prints: "User program completed!"                │
│                                                        │
│  sys_getticks(): Returns current timer ticks          │
│                                                        │
│  sys_sleep(ms): Sleep for milliseconds                │
│                                                        │
└────────────────────────────────────────────────────────┘
```

---

## 3. GDT Segment Configuration

### 3.1 User Mode Segment Selectors

```
GDT Entry Layout:

Entry 0 (0x00):     NULL
Entry 1 (0x08):     Kernel Code Segment (Ring 0)
Entry 2 (0x10):     Kernel Data Segment (Ring 0)
Entry 3 (0x18):     User Code Segment Base (Ring 0)
Entry 4 (0x20):     User Data Segment Base (Ring 0)
Entry 5 (0x28):     TSS (Task State Segment)


Selector Format:  ┌─────────────────┬───────┐
                  │  INDEX (13 bits) │ RPL(2)|
                  └─────────────────┴───────┘

User Code Selector:
  Raw:     0x18 (entry 3 in GDT)
  With RPL: 0x18 | 3 = 0x1B
  Binary:  0001_1011
           ├──────┬─ 3 (Ring 3)
           └────── Entry 3 (user code)

User Data Selector:
  Raw:     0x20 (entry 4 in GDT)
  With RPL: 0x20 | 3 = 0x23
  Binary:  0010_0011
           ├──────┬─ 3 (Ring 3)
           └────── Entry 4 (user data)


GDT Privilege Checks:

  For instruction like "mov ds, ax":
  ┌─────────────────────────────────────┐
  │ 1. Extract RPL from selector        │
  │ 2. Check: CPL (current) ≤ DPL (GDT)│
  │                                     │
  │ Example:                            │
  │   CPU in Ring 0, trying to load     │
  │   selector 0x23 (DPL=3)?            │
  │                                     │
  │   Error! Can only load Ring ≥ CPL   │
  │   (Actually, can only load selector │
  │    with RPL ≤ CPL for data segs)    │
  │                                     │
  │ This prevents Ring 3 from loading   │
  │   kernel code/data selectors        │
  │                                     │
  └─────────────────────────────────────┘
```

### 3.2 User Segment Descriptor Details

| Field | User Code (0x18) | User Data (0x20) | Meaning |
|-------|------------------|-----------------|---------|
| **Limit** | 0xFFFFF | 0xFFFFF | Max offset in segment |
| **Base** | 0x0 | 0x0 | Linear address base |
| **Present** | 1 | 1 | Descriptor valid |
| **DPL** | 3 | 3 | Required privilege level (Ring 3) |
| **Type** | 0x0A (code) | 0x02 (data) | Segment type |
| **L** | 1 | N/A | Long mode (64-bit) |
| **Granularity** | 1 | 1 | Limit in 4KB pages |

---

## 4. User Mode Entry Sequence

### 4.1 Setup Phase: Prepare User Program in Memory

```c
void test_user_program(void) {
    
    // Step 1: Get user program binary (linked at kernel compile time)
    extern char user_program_start[];
    extern char user_program_end[];
    uint64_t user_code_size = user_program_end - user_program_start;
    
    // user_program_start = 0xffffffff80???? (kernel high memory)
    // user_code_size = ~500 bytes (example)
    
    
    // Step 2: Allocate physical memory for user code
    void* user_code_phys = pmm_alloc_page();  // 4KB page
    // Returns: physical address like 0x1234000 (below 4GB)
    
    // Step 3: Copy user code from kernel memory to physical page
    {
        uint8_t* src = (uint8_t*)user_program_start;
        uint8_t* dst = (uint8_t*)user_code_phys;
        for (uint64_t i = 0; i < user_code_size; i++) {
            dst[i] = src[i];
        }
    }
    // Now physical page 0x1234000 contains user code
    
    
    // Step 4: Map physical page into user virtual address space
    uint64_t user_code_virt = 0x60000000;  // User preferred address
    vmm_map_page(user_code_virt,           // Virtual address
                 (uint64_t)user_code_phys, // Physical address
                 PAGE_PRESENT |            // Entry is valid
                 PAGE_WRITABLE |           // Writable
                 PAGE_USER);               // USER MODE ACCESS
    
    // Result in page table:
    //   PTE[0x60000000] = 0x1234007
    //   Bit 3 (PAGE_USER) = 1  ← Key for Ring 3!
    //   Bit 2 (WRITABLE) = 1
    //   Bit 0 (PRESENT) = 1
    
    
    // Step 5: Allocate user stack
    uint64_t user_stack_virt = 0x70000000;
    for (uint64_t i = 0; i < 2; i++) {
        void* phys = pmm_alloc_page();     // 4KB per page
        vmm_map_page(user_stack_virt + i*4096,
                     (uint64_t)phys,
                     PAGE_PRESENT |
                     PAGE_WRITABLE |
                     PAGE_USER);
    }
    // Stack occupies: 0x70000000 - 0x70008000 (8KB total)
    uint64_t user_stack_top = 0x70000000 + (2 * 4096) - 16;
    // Stack pointer initialized to: 0x70007FF0 (16-byte aligned, below top)
    
    
    // Step 6: Enter user mode
    enter_usermode(user_code_virt,    // RDI = 0x60000000 (entry point)
                   user_stack_top,    // RSI = 0x70007FF0 (sp top)
                   0);                // RDX = 0 (argument)
}
```

### 4.2 Transition Phase: enter_usermode() Assembly

```asm
enter_usermode(uint64_t entry, uint64_t stack, uint64_t arg):
    ; RDI = entry point (user code address)
    ; RSI = user stack pointer
    ; RDX = argument for user program

    Step 1: Load user segment registers (Ring 0 can do this)
    
        mov ax, USER_DS        ; 0x23 (user data segment RPL=3)
        mov ds, ax             ; DS ← 0x23
        mov es, ax             ; ES ← 0x23
        mov fs, ax             ; FS ← 0x23
        mov gs, ax             ; GS ← 0x23
        
        ; Now all data segments point to user ring 3
        ; (SS will be set by iretq from the frame)
    
    
    Step 2: Preserve registers in callee-saved regs
    
        mov r10, rdi           ; r10 = entry point
        mov r11, rsi           ; r11 = user stack
        mov r12, rdx           ; r12 = argument
    
    
    Step 3: Build iretq frame on CURRENT kernel stack
    
        push qword USER_DS     ; SS  = 0x23 (user stack segment)
        push r11               ; RSP = user stack pointer
        
        pushfq                 ; Push current RFLAGS
        pop rax                ; RAX = RFLAGS
        or rax, 0x0200         ; Set RFLAGS.IF = 1 (enable interrupts)
        push rax               ; Push modified RFLAGS
        
        push qword USER_CS     ; CS  = 0x1B (user code segment)
        push r10               ; RIP = entry point
    
    
    Step 4: Set argument for user program
    
        mov rdi, r12           ; RDI = argument (first param)
    
    
    Step 5: Execute return to user mode
    
        iretq                  ; Return to user mode!
                               ; Never returns to here
```

---

## 5. User Program Execution

### 5.1 Simple User Program Structure

```asm
; user_program.asm - Standalone user mode program

bits 64
section .user_text

global user_program_start

user_program_start:
    ; Running in Ring 3 (unprivileged user mode)
    
    ; Set segment registers for safety
    mov ax, 0x23           ; USER_DS (0x20 | 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Syscall 1: Write (SYS_WRITE)
    ; Arguments:
    ;   RAX = 1 (syscall number)
    ;   RDI = 1 (file descriptor, 1=STDOUT)
    ;   RSI = address of "Hello from user mode!\n"
    ;   RDX = 23 (length in bytes)
    
    lea rsi, [rel msg_hello]    ; RIP-relative addressing
    mov rdx, 23
    mov rax, 1                  ; SYS_WRITE
    mov rdi, 1                  ; STDOUT
    syscall                      ; Invoke kernel!
                                 ; Returns here after kernel processes
    
    ; Loop: Count from 0 to 9 and print each
    xor r15, r15                ; r15 = 0 (counter)
    
.loop:
    ; Save counter
    push r15
    
    ; Print "Count: "
    lea rsi, [rel msg_count]
    mov rdx, 7
    mov rax, 1
    mov rdi, 1
    syscall                      ; SYS_WRITE "Count: "
    
    ; Restore and print counter digit
    pop r15
    push r15
    lea rsi, [rel digits]
    add rsi, r15                 ; Point to digit
    mov rdx, 1
    mov rax, 1
    mov rdi, 1
    syscall                      ; SYS_WRITE one digit
    
    ; Print newline
    lea rsi, [rel newline_msg]
    mov rdx, 1
    mov rax, 1
    mov rdi, 1
    syscall
    
    ; Delay loop (busy wait)
    mov r14, 100000
.delay:
    dec r14
    jnz .delay
    
    ; Increment counter, check limit
    pop r15
    inc r15
    cmp r15, 10
    jl .loop
    
    ; Exit syscall (syscall 60: SYS_EXIT)
    xor rax, rax              ; RAX = 0 (SYS_EXIT)
    xor rdi, rdi              ; RDI = 0 (exit code)
    syscall                    ; Call kernel
    
    jmp $                      ; Should never reach here
    
; Data section (included in same binary)
msg_hello:
    db "Hello from user mode!", 10
    
msg_count:
    db "Count: "
    
digits:
    db "0123456789"
    
newline_msg:
    db 10

global user_program_end
user_program_end:
```

### 5.2 Syscall Execution Flow (User Perspective)

```
User Program (Ring 3):                Kernel (Ring 0):
                                       
Running at 0x60000000:                 
    syscall ──────────────────────────┐
    (saved RIP→RCX,                   │
     saved RFLAGS→R11)                │
                                       ▼
                                    syscall_entry:
                                       - Push RCX (user RIP)
                                       - Push R11 (user RFLAGS)
                                       - Save all registers
                                       - Call syscall_handler(RAX, ...)
                                       
                                       Handler checks RAX:
                                       
                    1 (SYS_WRITE)? ─→ sys_write(fd, buf, len)
                                       - Writes to terminal
                                       - Returns byte count in RAX
                                       
                    60 (SYS_EXIT)? ─→ sys_exit(code)
                                       - Prints "exit()"
                                       - Halts system
                                       
                                       Return path:
                                       - RAX = result
                                       - sysret instruction:
                                         * Load RIP ← RCX
                                         * Load RFLAGS ← R11
                                         * Switch to Ring 3
                                       
Resuming in user code:  ◄────────────── Return from syscall
    (instruction after syscall)        RAX has result
    
    Execution continues in user mode
```

---

## 6. Files Added/Modified

### 6.1 New Files (3 files)

| File | Description | Key Content |
|------|-------------|-------------|
| **`kernel/include/user.h`** | User mode header | `#define USER_CS, USER_DS` and `enter_usermode()` declaration |
| **`kernel/arch/usermode.asm`** | Ring transition code | `enter_usermode()` implementation using `iretq` |
| **`kernel/user/user_program.asm`** | Sample user program | Self-contained user code with syscalls |

### 6.2 Modified Files (7 files)

| File | Changes |
|------|---------|
| **`kernel/kernel.c`** | Added `test_user_program()` function; includes `#include <user.h>` |
| **`kernel/arch/syscall_asm.asm`** | Updated comments and added proper syscall frame handling |
| **`kernel/syscall/syscall.c`** | Modified STAR MSR setup for proper Ring transitions; updated `sys_exit()` to print completion message |
| **`kernel/include/mm/vmm.h`** | Added `vmm_dump_page_tables()` for debugging page mappings |
| **`kernel/mm/vmm.c`** | Implemented `vmm_dump_page_tables()` for user memory verification |
| **`linker.ld`** | Added `.user_text` and `.user_data` sections for user program |
| **`Makefile`** | Added `usermode.asm` and `user_program.asm` to build; added pattern rules for nested assembly files |

---

## 7. Core Implementation Details

### 7.1 enter_usermode() Implementation

**Location:** [kernel/arch/usermode.asm](kernel/arch/usermode.asm)

```asm
void enter_usermode(uint64_t entry, uint64_t stack, uint64_t arg)

Input Parameters:
  RDI (entry):  Virtual address where user program starts
                Example: 0x60000000
  
  RSI (stack):  User stack pointer (top of user stack)
                Example: 0x70007FF0 (16-byte aligned)
                Should be at or near 4K boundary
  
  RDX (arg):    First argument to user program (unused=0)
                Passed in RDI to user code

Process:
  1. Load user segment registers (DS, ES, FS, GS)
     These must be loaded while still in Ring 0
     User code cannot load segment registers!
     
  2. Build iretq frame:
     Frame size: 5 × 8 bytes = 40 bytes
     Format (after execution):
     [rsp+0]  = RIP (entry point)
     [rsp+8]  = CS (0x1B = Ring 3)
     [rsp+16] = RFLAGS (IF=1, flags preserved)
     [rsp+24] = RSP (user stack)
     [rsp+32] = SS (0x23 = Ring 3)
     
  3. Execute iretq
     CPU pops frame and switches privilege level
     Never returns to caller
```

**Key Implementation Detail:**

The function uses `iretq` instead of `sysret` because:

| Feature | iretq | sysret |
|---------|-------|--------|
| **Segment Control** | Full (pops CS, SS) | Limited (MSR-based) |
| **In 64-bit Mode** | Reliable | Segment issues |
| **User Stack Stack Switch** | Via frame (RSP) | Via MSR |
| **RFLAGS Restoration** | Via frame | Via MSR (R11) |
| **Reliability** | Better for usermode | Designed for kernel→user |

### 7.2 Memory Mapping with PAGE_USER

**Location:** [kernel/mm/vmm.c](kernel/mm/vmm.c)

```c
// User code page mapping
vmm_map_page(0x60000000,           // Virtual address
             (uint64_t)code_phys,  // Physical address
             PAGE_PRESENT      // 0x001 - Page is valid
           | PAGE_WRITABLE     // 0x002 - Page is writable
           | PAGE_USER);       // 0x004 - USER MODE ACCESS (Critical!)

// Page Table Entry (PTE) after mapping:
// Bits 63-12: Physical address (4K aligned)
// Bit 3 (PAGE_USER):    1 ← Ring 3 can access
// Bit 2 (WRITABLE):     1 ← Can write
// Bit 0 (PRESENT):      1 ← Page is present

// If Ring 3 tries to access page without PAGE_USER:
// → #PF (Page Fault), privilege violation
// → Exception handler triggered
// → Process terminated or exception delivered
```

---

## 8. Syscall Implementation Details

### 8.1 STAR MSR Configuration

**Location:** [kernel/syscall/syscall.c](kernel/syscall/syscall.c)

```c
// STAR (Segment Target Address Register)
// MSR 0xC0000081

uint64_t star_val = ((uint64_t)0x08 << 48)  // Bits 63:48 - Kernel CS
                  | ((uint64_t)0x08 << 32); // Bits 47:32 - Kernel CS again

wrmsr(IA32_STAR, star_val);

Interpretation:
  - Bits 63:48 = 0x08 (Kernel CS for sysret)
    When CPU executes sysret:
      sysret CS = STAR[63:48] + 16 = 0x08 + 16 = 0x18
      sysret CS with RPL = 0x18 | 3 = 0x1B (User CS) ✓
      
  - Bits 47:32 = 0x08 (Kernel CS for syscall)
    When CPU executes syscall:
      syscall CS = STAR[47:32] = 0x08 (Kernel CS) ✓
      syscall SS = STAR[47:32] + 8 = 0x10 (Kernel DS) ✓

This configuration enables seamless Ring 3 ↔ Ring 0 transitions!
```

### 8.2 System Call Handler

**Location:** [kernel/syscall/syscall.c](kernel/syscall/syscall.c)

```c
uint64_t syscall_handler(uint64_t sys_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    
    switch (sys_num) {
        case SYS_WRITE:     // 1
            // arg1 = fd (1 = STDOUT)
            // arg2 = buffer address (user space!)
            // arg3 = byte count
            return sys_write(arg1, arg2, arg3);
            
        case SYS_EXIT:      // 60
            // arg1 = exit code
            return sys_exit(arg1, 0, 0, 0, 0);
            
        case SYS_GETTICKS:  // 95
            return sys_getticks(0, 0, 0, 0, 0);
            
        case SYS_SLEEP:     // 57
            // arg1 = milliseconds
            return sys_sleep(arg1, 0, 0, 0, 0);
            
        default:
            terminal_writestring("[SYSCALL] Unknown syscall: ");
            uint64_to_string(sys_num, buf);
            terminal_writestring(buf);
            terminal_writestring("\n");
            return -1;      // Error
    }
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd == 1) {  // STDOUT
        const char* ptr = (const char*)buf;
        for (uint64_t i = 0; i < len; i++) {
            if (ptr[i] != 0) {
                terminal_putchar(ptr[i]);
            }
        }
        return len;
    }
    return -1;
}

static uint64_t sys_exit(uint64_t code, ...) {
    terminal_writestring("[SYSCALL] exit(");
    uint64_to_string(code, buf);
    terminal_writestring(buf);
    terminal_writestring(") called - user program completed!\n");
    terminal_writestring("\n=== USER MODE TEST SUCCESSFUL ===\n\n");
    
    for(;;) {
        __asm__ volatile("hlt");  // Halt
    }
}
```

---

## 9. Protection Mechanisms

### 9.1 Memory Access Control

| Condition | Scenario | Result |
|-----------|----------|--------|
| **Ring 3 access page with PAGE_USER=1** | User reads/writes user memory | ✓ Allowed |
| **Ring 3 access page with PAGE_USER=0** | User tries kernel memory access | ✗ #PF Exception |
| **Ring 0 access page with PAGE_USER=1** | Kernel reads/writes user memory | ✓ Allowed (intentional for syscalls) |
| **Ring 0 access page with PAGE_USER=0** | Kernel accesses kernel memory | ✓ Allowed |

### 9.2 Instruction Privilege Levels

| Instruction | Ring 0 | Ring 3 | Effect in Ring 3 |
|-------------|--------|--------|------------------|
| **mov ds, ax** | ✓ | ✗ | #GP (General Protection) |
| **iretq** | ✓ | ✗ | #GP (General Protection) |
| **cli** | ✓ | ✗ | #GP (General Protection) |
| **sti** | ✓ | ✗ | #GP (General Protection) |
| **mov cr3, rax** | ✓ | ✗ | #GP (General Protection) |
| **syscall** | ✓ | ✓ | ✓ Transitions to Ring 0 |
| **sysret** | ✓ | ✗ | #UD (Invalid Opcode) |
| **mov rax, [0x100]** | ✓ | ✓ | ✓ If page present and accessible |

---

## 10. Execution Flow Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│          Complete User Mode Lifecycle                            │
└──────────────────────────────────────────────────────────────────┘


Kernel Bootstrap (Ring 0):
    │
    ├─ Initialize GDT with user segments
    ├─ Initialize kernel memory (VMM, PMM, heap)
    ├─ Initialize interrupts and exceptions
    ├─ Initialize syscall MSRs (STAR, LSTAR, FMASK)
    │
    └─ test_user_program()
            │
            ├─ Allocate 4KB physical page
            ├─ Copy user_program binary from kernel memory
            ├─ Map at virtual 0x60000000 with PAGE_USER
            ├─ Allocate 8KB user stack
            ├─ Map at virtual 0x70000000 with PAGE_USER
            │
            ├─ Call enter_usermode(0x60000000, 0x70007FF0, 0)
            │       │
            │       ├─ Load DS,ES,FS,GS = 0x23 (user data)
            │       ├─ Build iretq frame with:
            │       │       RIP = 0x60000000
            │       │       CS = 0x1B
            │       │       RFLAGS = 0x200 (IF=1)
            │       │       RSP = 0x70007FF0
            │       │       SS = 0x23
            │       └─ iretq → transition to Ring 3
            │
            └─→ User Program Executes (Ring 3):
                    at 0x60000000
                    │
                    ├─ Set segment registers = 0x23
                    │
                    ├─ syscall (SYS_WRITE, "Hello from user mode!")
                    │   │
                    │   ├─ CPU: Save RIP→RCX, RFLAGS→R11
                    │   ├─ CPU: Load RIP from LSTAR (syscall_entry)
                    │   ├─ CPU: Load CS,SS from STAR
                    │   └─ CPU: Transition to Ring 0
                    │       │
                    │       ▼ (Back in Kernel, Ring 0)
                    │       syscall_handler():
                    │           case SYS_WRITE:
                    │               sys_write(1, buf, len)
                    │               → terminal_writestring()
                    │               → return bytes written
                    │       ┌─ sysret:
                    │       │   Load RIP ← RCX
                    │       │   Load RFLAGS ← R11
                    │       │   Transition to Ring 3
                    │       └─→ Back to user code
                    │
                    ├─ Loop and syscall 9 more times
                    │   (Print "Count: 0" through "Count: 9")
                    │
                    └─ syscall (SYS_EXIT, 0)
                        │
                        ├─ CPU: Transition to Ring 0
                        │
                        ▼ (Kernel)
                        sys_exit(0):
                            terminal_writestring("=== USER MODE TEST SUCCESSFUL ===")
                            for(;;) hlt;  ← Halt CPU
```

---

## 11. Testing & Verification

### 11.1 Test Procedure

```c
// In kernel/kernel.c, kernel_main():

// Phase 1: Boot and initialize all subsystems
pic_init();
timer_init();
syscall_init();
idt_enable_interrupts();

// Phase 2: Run all tests
test_timer();           // Verify timer and basic syscalls
test_keyboard();        // Verify keyboard input
test_context_switch();  // Verify process switching

// Phase 3: Final test - User mode execution
test_user_program();    // Launch user program in Ring 3
```

### 11.2 Expected Output

```
[TEST] User Program Test
[TEST] ========================
[USER] User program size: 428 bytes
[USER] user_program_start: 0xffffffff80??????
[USER] user_program_end:   0xffffffff80??????
[USER] Allocating physical page for user code...
[USER] Physical address: 0x1234000
[USER] Copying user code to physical page...
[USER] Mapping user code at virtual address 0x60000000
[OK] User code page mapped
[USER] Allocating user stack...
  Stack page 0 at virt 0x70000000
  Stack page 1 at virt 0x70001000
[USER] Stack top at 0x70007ff0
[DEBUG] Page table verification:
  Page table for 0x60000000:
    PML4E[24]=0x????? (valid)
    PDPE[0]=0x????? (valid)
    PDE[384]=0x????? (valid)
    PTE[0]=0x1234007 (present, writable, user)
[USER] Entering user mode...

Hello from user mode!
Count: 0
Count: 1
Count: 2
Count: 3
Count: 4
Count: 5
Count: 6
Count: 7
Count: 8
Count: 9

=== USER MODE TEST SUCCESSFUL ===
```

### 11.3 Verification Checklist

- [ ] GDT has user segments at 0x18 and 0x20
- [ ] User program is copied to physical memory
- [ ] Virtual pages mapped with PAGE_USER flag
- [ ] enter_usermode() builds correct iretq frame
- [ ] CPU transitions to Ring 3 (CS.RPL changes to 3)
- [ ] User program outputs visible on screen
- [ ] Syscalls execute correctly from Ring 3
- [ ] Kernel regains control after each syscall
- [ ] Program completes with exit syscall
- [ ] System halts without crash

---

## 12. Known Limitations & Future Enhancements

### 12.1 Current Limitations

| Limitation | Reason | Future Enhancement |
|-----------|--------|-------------------|
| **Single user program** | No process creation from Ring 3 | Implement `SYS_FORK`, `SYS_EXEC` |
| **No shared libraries** | Statically linked only | Implement dynamic linking, `SYS_MMAP` |
| **Minimal syscall interface** | Core features only | Add file I/O, process control, signals |
| **No virtual memory flexibility** | Fixed memory layout | Implement demand paging, ASLR |
| **No exception handling in Ring 3** | Not tested | Implement signal delivery to Ring 3 |
| **No inter-process communication** | Single process only | Implement pipes, sockets, shared memory |

### 12.2 Future Enhancements

1. **Process Creation (fork/exec)**
   - Allow user programs to create child processes
   - Implement copy-on-write memory optimization
   
2. **File System Syscalls**
   - `SYS_OPEN`, `SYS_CLOSE`, `SYS_READ`, `SYS_WRITE`
   - Support for file descriptors and the VFS layer
   
3. **Signal Handling**
   - Deliver signals to Ring 3 processes
   - User-installable signal handlers
   
4. **Virtual Memory Features**
   - Demand paging (load pages on access)
   - Memory mapping (mmap family)
   - Address space layout randomization (ASLR)
   
5. **Advanced Scheduling**
   - Priority-based scheduling (not just round-robin)
   - Sleeping processes (wakeup conditions)
   
6. **Security Enhancements**
   - Process isolation verification
   - Capability-based security model
   - SELinux-style mandatory access control

---

## 13. Technical References

### 13.1 x86-64 Privilege Mechanisms

| Topic | Key Concept | Section |
|-------|-------------|---------|
| **CPL (Current Privilege Level)** | From CS.RPL during execution | 2.1 |
| **DPL (Descriptor Privilege Level)** | In GDT descriptor, minimum CPL needed | Section 3 |
| **RPL (Request Privilege Level)** | In selector low 2 bits | 3.1 |
| **Privilege Checks** | Access allowed if CPL ≥ DPL (lower=more privileged) | 3.1 |
| **Ring Transition** | Change from CPL=0→3 or 3→0 via iretq/syscall | 2.2 |
| **Page User Bit** | Bit 2 in PTE, allows Ring 3 access | 7.2 |

### 13.2 Files by Function

| Purpose | Files |
|---------|-------|
| **GDT Setup** | kernel/arch/gdt.c, kernel/include/arch/gdt.h |
| **User Entry** | kernel/arch/usermode.asm, kernel/include/user.h |
| **Syscall Handler** | kernel/syscall/syscall.c, kernel/arch/syscall_asm.asm |
| **Memory Management** | kernel/mm/vmm.c, kernel/arch/paging_asm.asm |
| **User Program** | kernel/user/user_program.asm |

---

## 14. Troubleshooting Common Issues

### 14.1 User Program Doesn't Start

**Symptom:** System hangs or crashes when `enter_usermode()` is called

**Diagnosis Checklist:**
1. Verify GDT user segments exist (entries 3 and 4)
2. Check user pages are mapped with PAGE_USER=1
3. Verify stack pointer is within mapped region
4. Ensure RFLAGS.IF=1 in iretq frame (so interrupts work)
5. Check code is properly copied to physical page

**Fix:** Add debug output to `vmm_dump_page_tables(user_code_virt)`

### 14.2 Syscall Doesn't Return to User Code

**Symptom:** Syscall executes, but user program doesn't resume

**Diagnosis:**
1. Verify STAR MSR is correctly set
2. Check RCX has user RIP saved (valid address)
3. Verify R11 has user RFLAGS
4. Ensure syscall_entry properly restores registers

**Fix:** Add debug prints in syscall_entry and sys_exit

### 14.3 User Program Output Doesn't Appear

**Symptom:** Syscall executes but output is not visible

**Diagnosis:**
1. Verify fd=1 (STDOUT) in sys_write check
2. Check buffer address is in user memory
3. Verify terminal_writestring() works from Ring 0
4. Check buffer is not corrupted

**Fix:** Manually verify buffer contents with debugger

---

## 15. Summary

**Phase 10** successfully implements user mode execution, enabling:

✓ Safe, isolated user program execution in Ring 3  
✓ Seamless Ring 3 ↔ Ring 0 transitions via syscalls  
✓ Protected memory access with privilege checking  
✓ Foundation for multi-process operating system  

The implementation uses industry-standard x86-64 privilege mechanisms (GDT, page tables, syscall/sysret) with clear separation of user/kernel concerns. This provides a secure, extensible base for future user-space applications and kernel services.

---

## 16. Appendix: Assembly Reference

### Entry Point CPU State

When user program first executes:

| Register | Value | Meaning |
|----------|-------|---------|
| **RIP** | user_code_virt | Instruction pointer |
| **RSP** | user_stack_top | Stack pointer |
| **RDI** | arg (usually 0) | First argument |
| **CS** | 0x1B | User code segment (Ring 3) |
| **DS,ES,FS,GS** | 0x23 | User data segment (Ring 3) |
| **SS** | 0x23 | User stack segment (Ring 3) |
| **RFLAGS.IF** | 1 | Interrupts enabled |
| **RFLAGS.CF** | Variable | Carry flag (from kernel state) |
| **CR3** | Same | Current page table (unchanged) |

### User Program Linkage

```
Linker Script (.user_text section):
    ┌─────────────────────────────────┐
    │  user_program_start (symbol)    │  ← Exported as kernel symbol
    │                                 │
    │  .user_text and .user_data      │
    │  (actual instructions + data)   │
    │                                 │
    │  user_program_end (symbol)      │  ← Exported as kernel symbol
    └─────────────────────────────────┘
    
Kernel sees as:
    extern char user_program_start[];
    extern char user_program_end[];
    
Size calculation:
    user_code_size = user_program_end - user_program_start
```

---

---

## Appendices: Future Roadmap (v0.0.3)

### v0.0.3 Development Phases

MakhOS v0.0.3 will build upon the v0.0.2 foundation with advanced process management:

```
MakhOS v0.0.3 Development Plan
├── Phase 11: Process Core Enhancement
│   ├── Multi-process execution
│   ├── Process trees (parent-child relationships)
│   ├── Process groups
│   └── Better resource tracking
│
├── Phase 12: fork() System Call
│   ├── Process duplication
│   ├── File descriptor inheritance
│   ├── Copy-on-Write (CoW) mechanism
│   └── Process hierarchy
│
├── Phase 13: wait() and exit() Lifecycle
│   ├── Process synchronization
│   ├── Zombie process collection
│   ├── Exit code propagation
│   └── Parent-child synchronization
│
├── Phase 14: Priority Scheduler
│   ├── Weighted round-robin scheduling
│   ├── Nice levels and priority classes
│   ├── Dynamic priority adjustment
│   └── Load balancing
│
├── Phase 15: Process Isolation (Copy-on-Write)
│   ├── Memory sharing with write protection
│   ├── Page fault handling for CoW
│   ├── Private page creation on write
│   └── Memory efficiency optimization
│
├── Phase 16: Signals
│   ├── Signal delivery mechanism
│   ├── Signal handlers in user space
│   ├── kill() and pause() syscalls
│   ├── Signal masks and blocking
│   └── Real-time signals
│
└── Phase 17: IPC Foundation (Pipes)
    ├── Unnamed pipe creation
    ├── pipe() syscall
    ├── Data flow between processes
    ├── EOF handling
    └── Blocking I/O semantics
```

### v0.0.3 Goals

1. **Full Multitasking:** Multiple independent processes running simultaneously
2. **Process Control:** Complete fork/wait/exit lifecycle support
3. **Inter-Process Communication:** Pipes for data transfer between processes
4. **Signal Handling:** Asynchronous event delivery to user programs
5. **Advanced Scheduling:** Priority-based scheduling with dynamic adjustment
6. **Memory Efficiency:** Copy-on-Write for fork() optimization

**End of Phase 10 Documentation (v0.0.2 Final)**
