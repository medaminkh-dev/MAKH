; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 Amine Khemissi
; =============================================================================
; usermode.asm - ring-3 entry, syscall entry/exit, and uaccess copies (Phase 16)
; =============================================================================
bits 64
section .text

extern syscall_dispatch        ; C: uint64_t syscall_dispatch(trapframe_t* tf)

; -----------------------------------------------------------------------------
; enter_user_mode(rip=rdi, rsp=rsi) -> noreturn
; Drop to ring 3 via iretq, building the frame the CPU pops (deepest first):
;   SS, RSP, RFLAGS, CS, RIP.
; -----------------------------------------------------------------------------
global enter_user_mode
enter_user_mode:
    mov ax, 0x1B               ; SEL_USER_DATA
    mov ds, ax
    mov es, ax
    push 0x1B                  ; SS
    push rsi                   ; RSP
    push 0x202                 ; RFLAGS: IF=1 so the timer can preempt ring 3
                               ; (run_user_program stays non-preemptive via
                               ; preempt_disable; bit1 is the reserved set bit)
    push 0x23                  ; CS  (SEL_USER_CODE)
    push rdi                   ; RIP
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    iretq

; -----------------------------------------------------------------------------
; syscall_entry - IA32_LSTAR target. On entry (from `syscall` in ring 3):
;   RCX=user RIP, R11=user RFLAGS, RAX=number, args in RDI RSI RDX R10 R8 R9.
; No stack switch is automatic, so swapgs to the per-CPU block and move to the
; kernel stack first. We build a trapframe and hand its pointer to C; the
; dispatcher reads args and writes the result back into tf->rax.
; -----------------------------------------------------------------------------
global syscall_entry
syscall_entry:
    swapgs
    mov [gs:8], rsp            ; percpu.user_rsp_tmp = user RSP
    mov rsp, [gs:0]            ; percpu.kernel_rsp

    push qword 0x1B            ; tf.ss
    push qword [gs:8]          ; tf.rsp (user)
    push r11                   ; tf.rflags
    push qword 0x23            ; tf.cs
    push rcx                   ; tf.rip
    push rax                   ; tf.int_no (reuse for syscall number)
    push rax                   ; tf.rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15                   ; tf.r15 (lowest address = tf base)

    mov rbx, rsp               ; anchor the trapframe (rbx is callee-saved)
    ; NOTE: interrupts stay masked for the whole syscall in this bring-up
    ; (FMASK already cleared IF). Preemptible user mode comes in Phase 17.
    mov rdi, rbx               ; arg: trapframe_t*
    and rsp, -16               ; align for the C ABI
    call syscall_dispatch
    mov rsp, rbx               ; restore exactly to the trapframe

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax                    ; dispatcher wrote the return value here
    add rsp, 8                 ; skip tf.int_no
    pop rcx                    ; tf.rip -> RCX for sysret
    add rsp, 8                 ; skip tf.cs
    pop r11                    ; tf.rflags -> R11 for sysret
    pop rsp                    ; tf.rsp (user) ; tf.ss left abandoned on kstack
    swapgs
    o64 sysret

; -----------------------------------------------------------------------------
; __copy_user(dst=rdi, src=rsi, n=rdx) -> rax = bytes NOT copied (0 = success)
; A page fault in the copy body is redirected by the fault-fixup table to
; __copy_user_fault, which returns the remaining count. EFLAGS.AC is raised
; around the loop (via popfq, valid whether or not SMAP is enabled) so the
; kernel may touch user pages when SMAP is on.
; -----------------------------------------------------------------------------
global __copy_user
global __copy_user_fault_ip
global __copy_user_fault
__copy_user:
    mov rax, rdx
    test rdx, rdx
    jz __copy_user_done
    pushfq
    pop r8
    or  r8, (1 << 18)          ; set AC
    push r8
    popfq
__copy_user_fault_ip:
    mov cl, [rsi]              ; faults here on a bad user address
    mov [rdi], cl
    inc rsi
    inc rdi
    dec rax
    jnz __copy_user_fault_ip
__copy_user_fault:
    pushfq                     ; fixup resumes here with rax = bytes remaining
    pop r8
    and r8, ~(1 << 18)         ; clear AC
    push r8
    popfq
__copy_user_done:
    ret
