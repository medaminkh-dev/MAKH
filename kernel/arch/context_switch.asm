; =============================================================================
; context_switch.asm - Low-level context switching for MakhOS
; =============================================================================
; This file implements the core context switching mechanism for preemptive
; multitasking. It saves the current process's CPU state and restores another
; process's state.
;
; Function: void context_switch(context_t* old, context_t* new)
;   - old: Pointer to save current process context (can be NULL for first switch)
;   - new: Pointer to load new process context
;
; Register Usage (System V AMD64 ABI):
;   - rdi: first argument (old context)
;   - rsi: second argument (new context)
;
; Changes in Phase 9:
;   - NEW FILE: Core component of process management system
;
; =============================================================================

bits 64
section .text

; -----------------------------------------------------------------------------
; context_switch - Save/Load process context
; -----------------------------------------------------------------------------
; Saves current process state to 'old' context and loads 'new' context.
; This enables preemptive multitasking by switching between processes.
global context_switch
context_switch:
    ; ========================================================================
    ; PHASE 9 CHANGE: This is a NEW file implementing context switching
    ; The context_t structure (200 bytes) contains:
    ;   - General purpose registers (rax, rbx, rcx, rdx, rsi, rdi, rbp, r8-r15)
    ;   - Stack and instruction pointers (rsp, rip)
    ;   - Flags register (rflags)
    ;   - Page table register (cr3)
    ;   - Segment registers (cs, ds, es, fs, gs, ss)
    ; ========================================================================

    ; Save current context to 'old' (rdi)
    
    ; Save general purpose registers
    mov [rdi + 0], rax      ; rax
    mov [rdi + 8], rbx      ; rbx
    mov [rdi + 16], rcx     ; rcx
    mov [rdi + 24], rdx     ; rdx
    mov [rdi + 32], rsi     ; rsi (original)
    mov [rdi + 40], rdi     ; rdi (original)
    mov [rdi + 48], rbp     ; rbp
    
    mov [rdi + 56], r8      ; r8
    mov [rdi + 64], r9      ; r9
    mov [rdi + 72], r10     ; r10
    mov [rdi + 80], r11     ; r11
    mov [rdi + 88], r12     ; r12
    mov [rdi + 96], r13     ; r13
    mov [rdi + 104], r14    ; r14
    mov [rdi + 112], r15    ; r15
    
    ; Save stack pointer
    mov [rdi + 120], rsp    ; rsp
    
    ; Save instruction pointer (return address)
    mov rax, [rsp]          ; Return address is on stack
    mov [rdi + 128], rax    ; rip
    
    ; Save rflags
    pushfq
    pop qword [rdi + 136]   ; rflags
    
    ; Save CR3 (page table)
    mov rax, cr3
    mov [rdi + 144], rax    ; cr3
    
    ; Save segment registers
    mov rax, cs
    mov [rdi + 152], rax    ; cs
    mov rax, ds
    mov [rdi + 160], rax    ; ds
    mov rax, es
    mov [rdi + 168], rax    ; es
    mov rax, fs
    mov [rdi + 176], rax    ; fs
    mov rax, gs
    mov [rdi + 184], rax    ; gs
    mov rax, ss
    mov [rdi + 192], rax    ; ss
    
    ; At this point, rsi still points to new context
    ; rdi still points to old context
    
    ; ========================================================================
    ; Now load new context from 'new' (rsi)
    ; ========================================================================
    
    ; Save the new context pointer before we modify rsi
    mov rax, rsi            ; rax = new context pointer
    
    ; Load general purpose registers
    mov rax, [rsi + 0]      ; rax - restore original rax
    mov rbx, [rsi + 8]      ; rbx
    mov rcx, [rsi + 16]     ; rcx
    mov rdx, [rsi + 24]     ; rdx
    mov rbp, [rsi + 48]     ; rbp
    
    mov r8,  [rsi + 56]     ; r8
    mov r9,  [rsi + 64]     ; r9
    mov r10, [rsi + 72]     ; r10
    mov r11, [rsi + 80]     ; r11
    mov r12, [rsi + 88]     ; r12
    mov r13, [rsi + 96]     ; r13
    mov r14, [rsi + 104]    ; r14
    mov r15, [rsi + 112]    ; r15
    
    ; Load stack pointer - must be done before we lose rsi
    mov rsp, [rsi + 120]    ; rsp
    
    ; Load rflags
    push qword [rsi + 136]  ; rflags
    popfq
    
    ; Load CR3 (change page table)
    mov rax, [rsi + 144]    ; cr3
    mov cr3, rax
    
    ; Load segment registers
    mov rax, [rsi + 160]    ; ds
    mov ds, rax
    mov rax, [rsi + 168]    ; es
    mov es, rax
    mov rax, [rsi + 176]    ; fs
    mov fs, rax
    mov rax, [rsi + 184]    ; gs
    mov gs, rax
    
    ; Load rsi and rdi from new context - use rdx as temp
    ; We need to get them from the saved context before we jump
    mov rdx, [rsi + 32]     ; original rsi value
    mov rdi, [rsi + 40]     ; original rdi value
    
    ; Get new rip
    mov rsi, [rsi + 128]    ; rip (use rsi temporarily, will be overwritten)
    
    ; Jump to new instruction pointer
    push rsi                ; push rip to stack
    mov rsi, rdx            ; restore rsi
    ret                     ; pop rip and jump
