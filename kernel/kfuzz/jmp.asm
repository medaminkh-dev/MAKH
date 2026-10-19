; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 Amine Khemissi
; =============================================================================
; jmp.asm - setjmp/longjmp for the KFUZZ ring-0 sandbox (Phase 15)
; =============================================================================
; The freestanding kernel has no libc, so the self-fuzzer carries its own
; non-local jump. kfuzz_setjmp() saves the callee-saved registers, the stack
; pointer and the return address; kfuzz_longjmp() restores them, so a fault
; deep inside a fuzz target unwinds straight back to the harness instead of
; taking down the kernel.
;
;   int  kfuzz_setjmp(kfuzz_jmp_buf buf);      -> 0 the first time,
;                                                 the longjmp value on return
;   void kfuzz_longjmp(kfuzz_jmp_buf buf, int val);
;
; jmp_buf layout (9 qwords): rbx rbp r12 r13 r14 r15 rsp ret rflags
; =============================================================================

global kfuzz_setjmp
global kfuzz_longjmp

kfuzz_setjmp:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]          ; caller's rsp (after our return)
    mov [rdi + 48], rax
    mov rax, [rsp]              ; return address
    mov [rdi + 56], rax
    pushfq                      ; caller's RFLAGS (captures its IF for longjmp)
    pop rax
    mov [rdi + 64], rax
    xor eax, eax               ; first return: 0
    ret

kfuzz_longjmp:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov eax, esi               ; return value...
    test eax, eax
    jnz .go
    mov eax, 1                 ; ...never 0 (longjmp(buf,0) returns 1)
.go:
    ; A fault is taken through an interrupt gate, which clears IF. Recovery must
    ; land in exactly the IF state setjmp captured, not blindly enable: if the
    ; harness was in a critical section (IF=0) at setjmp, re-enabling here would
    ; reopen it early (see irq.h: restore the saved flags, never a bare sti).
    test qword [rdi + 64], 0x200   ; IF (bit 9) of the saved RFLAGS
    jz .irq_off
    sti
    jmp [rdi + 56]
.irq_off:
    cli
    jmp [rdi + 56]
