; syscall_asm.asm - x86_64 syscall entry/exit using syscall/sysret
bits 64
section .text

; External C handler
extern syscall_handler

; MSR addresses for syscall setup
%define IA32_STAR   0xC0000081
%define IA32_LSTAR  0xC0000082
%define IA32_FMASK  0xC0000084

; syscall_entry - Called by CPU on syscall instruction
global syscall_entry
syscall_entry:
    ; CPU automatically on syscall:
    ; - Saves RCX into RIP (return address)
    ; - Saves RFLAGS into R11
    ; - Switches to Ring 0
    ; - Loads CS from STAR MSR (bits 63:48)
    ; - Loads SS from STAR MSR (CS + 8)
    
    ; Save registers (System V ABI callee-saved)
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
    push rcx
    push rbx
    
    ; Save original stack pointer
    mov rbp, rsp
    
    ; Align stack to 16 bytes (for C ABI)
    and rsp, -16
    
    ; Arguments are passed in registers per System V ABI:
    ; RAX = syscall number (moved to RDI for C handler)
    ; RDI = arg1, RSI = arg2, RDX = arg3
    ; R10 = arg4, R8 = arg5, R9 = arg6
    ;
    ; CRITICAL: RCX and R11 are clobbered by syscall instruction
    ; RCX = saved RIP (return address)
    ; R11 = saved RFLAGS
    ; We must preserve RCX/R11 on stack before using them
    
    ; Save return address (RCX) and RFLAGS (R11) to stack
    ; They will be restored before sysret
    push rcx            ; Save return address
    push r11            ; Save RFLAGS
    
    ; Set up arguments for syscall_handler C function
    mov r9, r8          ; arg5
    mov r8, r10         ; arg4 (R10 in syscall ABI)
    mov rcx, rdx        ; arg3
    mov rdx, rsi        ; arg2
    mov rsi, rdi        ; arg1
    mov rdi, rax        ; syscall number
    
    call syscall_handler
    
    ; Restore RFLAGS (R11) and return address (RCX) first
    ; They were pushed after setting rbp, so restore them before restoring rbp
    pop r11
    pop rcx
    
    ; Restore stack (this restores the original rsp before alignment)
    mov rsp, rbp
    
    ; Restore registers in reverse order of push
    pop rbx
    pop rcx
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
    
    ; Return to user mode with sysret
    ; CPU automatically:
    ; - Restores RIP from RCX
    ; - Restores RFLAGS from R11
    ; - Loads CS and SS from STAR MSR (user segments)
    o64 sysret        ; Use o64 prefix for 64-bit sysret (sysretq)
