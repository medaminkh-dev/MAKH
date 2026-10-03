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
; jmp_buf layout (8 qwords): rbx rbp r12 r13 r14 r15 rsp ret
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
    ; A longjmp always recovers from a trap taken through an interrupt gate,
    ; which cleared IF. Re-enable interrupts before resuming the harness.
    sti
    jmp [rdi + 56]
