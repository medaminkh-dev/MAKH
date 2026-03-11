; =============================================================================
; syscall_asm.asm - x86_64 syscall entry/exit using syscall/iretq
; =============================================================================
; FIXES APPLIED:
;   BUG#2: syscall does NOT switch stacks. Now switches to kernel stack via
;          kernel_syscall_rsp global variable (set by set_kernel_syscall_rsp)
;   BUG#3: iretq frame had HARDCODED user RSP (0x70001FF0) - now uses real RSP
;   BUG#4: 'xor r9, r10' was wrong → fixed to 'xor r9, r9'
; =============================================================================
;
; SYSCALL ARGUMENT CONVENTION (Linux-compatible):
;   RAX = syscall number
;   RDI = arg1,  RSI = arg2,  RDX = arg3
;   R10 = arg4,  R8  = arg5,  R9  = arg6
;
; REGISTERS CLOBBERED BY syscall instruction:
;   RCX <- user RIP (return address)
;   R11 <- user RFLAGS
;
; =============================================================================

bits 64
section .text

extern syscall_handler

; kernel_syscall_rsp: global kernel stack top for syscall entry
; Must be updated every time we switch to a new process (set_kernel_syscall_rsp)
global kernel_syscall_rsp
extern kernel_syscall_rsp

global syscall_entry
syscall_entry:
    ; -------------------------------------------------------------------------
    ; FIX BUG#2: RSP is still USER stack here. Switch to kernel stack.
    ; We temporarily use r15 to hold user RSP (r15 is callee-saved, OK).
    ; -------------------------------------------------------------------------
    mov r15, rsp                        ; r15 = save user RSP
    mov rsp, [rel kernel_syscall_rsp]   ; switch to kernel stack

    ; -------------------------------------------------------------------------
    ; Now on kernel stack. Save everything needed.
    ; -------------------------------------------------------------------------

    ; Push user RSP first (needed later for iretq frame) - FIX BUG#3
    push r15                ; [+120] user RSP

    ; Push user RIP and RFLAGS (clobbered by syscall)
    push rcx                ; [+112] user RIP
    push r11                ; [+104] user RFLAGS

    ; Push callee-saved registers
    push rbx                ; [+96]
    push rbp                ; [+88]
    push r12                ; [+80]
    push r13                ; [+72]
    push r14                ; [+64]
    push r15                ; [+56]  r15 value (= user RSP, but that's fine,
                            ;         restoring it won't hurt since we fix RSP via iretq)

    ; Push syscall arguments (for passing to C handler)
    push rdi                ; [+48]  arg1
    push rsi                ; [+40]  arg2
    push rdx                ; [+32]  arg3
    push r10                ; [+24]  arg4
    push r8                 ; [+16]  arg5
    push r9                 ; [+8]   arg6
    push rax                ; [+0]   syscall number  ← rsp points here

    ; Save frame pointer for offset-based access
    mov rbp, rsp

    ; Align to 16 bytes for C ABI
    and rsp, -16

    ; Set up args: syscall_handler(num, arg1, arg2, arg3, arg4, arg5)
    mov rdi, [rbp + 0]      ; rdi = syscall number
    mov rsi, [rbp + 48]     ; rsi = arg1 (original rdi)
    mov rdx, [rbp + 40]     ; rdx = arg2 (original rsi)
    mov rcx, [rbp + 32]     ; rcx = arg3 (original rdx)
    mov r8,  [rbp + 24]     ; r8  = arg4 (original r10)
    mov r9,  [rbp + 16]     ; r9  = arg5 (original r8)

    call syscall_handler
    ; rax = return value (keep it)

    ; Restore rsp (undo alignment)
    mov rsp, rbp

    ; Pop args (skip rax = syscall number, we keep rax = return value)
    add rsp, 8              ; skip saved rax
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi

    ; Pop callee-saved registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Pop user RFLAGS and user RIP
    pop r11                 ; user RFLAGS
    pop rcx                 ; user RIP

    ; Pop saved user RSP - FIX BUG#3
    pop r15                 ; r15 = user RSP (was pushed at entry)

    ; -------------------------------------------------------------------------
    ; Build iretq frame on kernel stack to return to user mode
    ; -------------------------------------------------------------------------
    push qword 0x23         ; SS   = user data (GDT[4]|RPL3)
    push r15                ; RSP  = user stack pointer  ← FIX BUG#3 (was hardcoded!)
    push r11                ; RFLAGS
    or qword [rsp], 0x200   ; ensure IF=1
    push qword 0x1B         ; CS   = user code (GDT[3]|RPL3)
    push rcx                ; RIP  = user return address

    ; Clear registers before user mode
    xor rbx, rbx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8,  r8
    xor r9,  r9             ; FIX BUG#4: was 'xor r9, r10' (wrong!)
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq


; =============================================================================
; set_kernel_syscall_rsp(uint64_t rsp)
; Called from tss_set_kernel_stack() and process switch to update kernel stack
; =============================================================================
section .data
global kernel_syscall_rsp
kernel_syscall_rsp:
    dq 0

section .text
global set_kernel_syscall_rsp
set_kernel_syscall_rsp:
    mov [rel kernel_syscall_rsp], rdi
    ret
