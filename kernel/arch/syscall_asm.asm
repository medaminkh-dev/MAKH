; =============================================================================
; syscall_asm.asm - x86_64 syscall entry/exit using syscall/iretq
; =============================================================================
;
; We use iretq instead of sysret because sysret has issues with segment
; registers in 64-bit mode. With iretq, we can properly control all segments.
;
; SYSCALL ARGUMENT CONVENTION (Linux-compatible):
;   RAX = syscall number
;   RDI = arg1
;   RSI = arg2
;   RDX = arg3
;   R10 = arg4  (NOT RCX because syscall clobbers RCX!)
;   R8  = arg5
;   R9  = arg6
;
; RETURN VALUE:
;   RAX = return value
;
; REGISTERS CLOBBERED BY syscall instruction:
;   RCX <- user RIP
;   R11 <- user RFLAGS
;
; =============================================================================

bits 64
section .text

extern syscall_handler

global syscall_entry
syscall_entry:
    ; =========================================================================
    ; ENTRY STATE:
    ;   RSP = still user stack (syscall does NOT switch stack)
    ;   RCX = user RIP (return address) - MUST save
    ;   R11 = user RFLAGS                - MUST save
    ;   RAX = syscall number
    ;   RDI = arg1, RSI = arg2, RDX = arg3
    ;   R10 = arg4, R8  = arg5, R9  = arg6
    ;   CS = kernel code segment (0x08) due to STAR MSR
    ;   SS = kernel data segment (0x10) due to STAR MSR
    ; =========================================================================

    ; First: save user RIP and RFLAGS on kernel stack
    push rcx                ; user RIP
    push r11                ; user RFLAGS
    
    ; Save all callee-saved + arg registers
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Save syscall args
    push rdi                ; arg1
    push rsi                ; arg2
    push rdx                ; arg3
    push r10                ; arg4
    push r8                 ; arg5
    push r9                 ; arg6
    push rax                ; syscall number

    ; Align stack to 16 bytes
    mov rbp, rsp
    and rsp, -16

    ; Set up arguments for syscall_handler
    mov rdi, [rbp + 0]      ; rdi = syscall number
    mov rsi, [rbp + 48]     ; rsi = arg1
    mov rdx, [rbp + 40]     ; rdx = arg2
    mov rcx, [rbp + 32]     ; rcx = arg3
    mov r8,  [rbp + 24]     ; r8  = arg4
    mov r9,  [rbp + 16]     ; r9  = arg5

    call syscall_handler
    ; RAX = return value

    ; Restore RSP (undo alignment)
    mov rsp, rbp

    ; Restore saved registers (in reverse order)
    add rsp, 8              ; skip saved rax (return value)
    pop r9                  ; arg6
    pop r8                  ; arg5
    pop r10                 ; arg4
    pop rdx                 ; arg3
    pop rsi                 ; arg2
    pop rdi                 ; arg1

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ; Pop user RFLAGS and user RIP
    pop r11                 ; r11 = user RFLAGS
    pop rcx                 ; rcx = user RIP

    ; =========================================================================
    ; Use iretq to return to user mode instead of sysret
    ; iretq frame (from low to high, rsp points to RIP):
    ;   [rsp + 0]  = RIP (user return address)
    ;   [rsp + 8]  = CS  (user code segment = 0x1B)
    ;   [rsp + 16] = RFLAGS (user flags)
    ;   [rsp + 24] = RSP (user stack - we stay on user stack)
    ;   [rsp + 32] = SS (user data segment = 0x23)
    ; =========================================================================
    
    ; Build iretq frame on current (user) stack
    ; Since we're still on user stack, we push to build the frame
    
    push qword 0x23         ; SS - user data segment
    push qword 0x70001FF0  ; RSP - user stack (temporary, stays same)
    push r11                ; RFLAGS - user flags
    push qword 0x1B         ; CS - user code segment
    push rcx                ; RIP - user return address
    
    ; Enable interrupts in user mode
    or qword [rsp + 16], 0x200
    
    ; Clear other registers before returning to user
    xor rax, rax
    xor rbx, rbx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8, r8
    xor r9, r10
    xor r11, r11
    xor r12, r12
    
    ; Return to user mode
    iretq
