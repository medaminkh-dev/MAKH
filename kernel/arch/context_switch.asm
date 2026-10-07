; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 Amine Khemissi
; =============================================================================
; context_switch.asm - Low-level context switching for MakhOS
; =============================================================================
; void context_switch(context_t* old, context_t* new)
;   rdi = old context pointer (NULL = first switch, skip save)
;   rsi = new context pointer
;
; BUG FIXES:
;   BUG 1 - Interrupt window (fixed)
;   BUG 2 - rdx overwrite (fixed)
;   BUG 3 - IF=0 poisoning saved rflags (fixed: OR 0x200)
;   BUG 4 - push below new RSP → corruption (fixed: use iretq)
;   BUG 5 - RSP saved as P-8 instead of P  ← THIS FIX
;
;     "call context_switch" decrements RSP by 8 (pushes return addr).
;     Saving RSP as-is gives P-8. When iretq restores RSP=P-8, proc_yield
;     runs with RSP one slot too deep. Its epilogue reads ret-address from
;     the wrong slot → jumps to garbage → GPF.
;
;     Fix: save RSP+8 (undo the 8-byte decrement from the call instruction).
;     First-time process setups (idle, init, proc_create) set context.rsp
;     manually to the intended starting RSP — they are unaffected.
; =============================================================================

bits 64
section .text

global context_switch
context_switch:

    cli

    ; =========================================================================
    ; SAVE old context (skip if rdi == NULL)
    ; =========================================================================
    test rdi, rdi
    jz   .load

    mov [rdi +   0], rax
    mov [rdi +   8], rbx
    mov [rdi +  16], rcx
    mov [rdi +  24], rdx
    mov [rdi +  32], rsi
    mov [rdi +  40], rdi
    mov [rdi +  48], rbp
    mov [rdi +  56], r8
    mov [rdi +  64], r9
    mov [rdi +  72], r10
    mov [rdi +  80], r11
    mov [rdi +  88], r12
    mov [rdi +  96], r13
    mov [rdi + 104], r14
    mov [rdi + 112], r15

    ; RSP: save RSP+8, NOT RSP.
    ;
    ; At this point RSP = P-8 because "call context_switch" pushed 8 bytes.
    ; iretq restores RSP directly from context.rsp (no automatic +8 like ret).
    ; Saving P-8 would make the resumed process run with RSP one slot too deep,
    ; causing its epilogue to ret to the wrong address.
    ; Saving P (= RSP+8) makes iretq restore the same RSP that a normal
    ; ret from context_switch would have left behind.
    lea  rax, [rsp + 8]
    mov  [rdi + 120], rax

    ; RIP: the return address that "call" pushed at [rsp]
    mov  rax, [rsp]
    mov  [rdi + 128], rax

    ; RFLAGS: force IF=1 before saving.
    ; cli above cleared IF. Saving IF=0 would make hlt freeze on resume.
    pushfq
    pop  rax
    or   rax, 0x200
    mov  [rdi + 136], rax

    ; CR3
    mov  rax, cr3
    mov  [rdi + 144], rax

    ; Segment registers
    mov  rax, cs
    mov  [rdi + 152], rax
    mov  rax, ds
    mov  [rdi + 160], rax
    mov  rax, es
    mov  [rdi + 168], rax
    mov  rax, fs
    mov  [rdi + 176], rax
    mov  rax, gs
    mov  [rdi + 184], rax
    mov  rax, ss
    mov  [rdi + 192], rax

    ; =========================================================================
    ; LOAD new context via IRETQ
    ; =========================================================================
    ;
    ; Build iretq frame on the CURRENT (old) stack — no write to new stack.
    ; iretq in 64-bit mode always pops: RIP, CS, RFLAGS, RSP, SS.
    ;
    ; Stack layout after pushes (top = lowest address = first popped):
    ;   [RSP+0]  = RIP    ← popped 1st
    ;   [RSP+8]  = CS
    ;   [RSP+16] = RFLAGS
    ;   [RSP+24] = RSP_new (= P, the correct restored stack pointer)
    ;   [RSP+32] = SS     ← pushed 1st (deepest)
    ;
.load:
    push qword [rsi + 192]    ; SS      (pushed first)
    push qword [rsi + 120]    ; RSP_new (= P, saved as RSP+8)
    push qword [rsi + 136]    ; RFLAGS  (IF=1 guaranteed)
    push qword [rsi + 152]    ; CS
    push qword [rsi + 128]    ; RIP     (pushed last = top)

    ; Load general-purpose registers
    mov  rbx, [rsi +   8]
    mov  rcx, [rsi +  16]
    mov  rdx, [rsi +  24]
    mov  rbp, [rsi +  48]
    mov  r8,  [rsi +  56]
    mov  r9,  [rsi +  64]
    mov  r10, [rsi +  72]
    mov  r11, [rsi +  80]
    mov  r12, [rsi +  88]
    mov  r13, [rsi +  96]
    mov  r14, [rsi + 104]
    mov  r15, [rsi + 112]

    ; CR3
    mov  rax, [rsi + 144]
    mov  cr3, rax

    ; Segment registers. NOTE: GS is deliberately NOT reloaded here. Reloading
    ; the gs selector resets the active GS base to 0, which breaks the swapgs
    ; invariant when resuming a thread that blocked mid-syscall (it would unwind
    ; to syscall_return's swapgs with the bases the wrong way round and leave
    ; ring 3 with KERNEL_GS_BASE = 0, crashing its next syscall). The GS base is
    ; owned entirely by arch_prepare_switch (it pins both bases to the per-CPU
    ; block on every switch) and by swapgs; context_switch must not touch it.
    mov  rax, [rsi + 160]
    mov  ds, rax
    mov  rax, [rsi + 168]
    mov  es, rax
    mov  rax, [rsi + 176]
    mov  fs, rax

    ; Load rax, rdi, rsi last (destroys context pointer)
    mov  rax, [rsi +   0]
    mov  rdi, [rsi +  40]
    mov  rsi, [rsi +  32]

    ; Atomically restore RIP + CS + RFLAGS (IF=1) + RSP + SS
    iretq