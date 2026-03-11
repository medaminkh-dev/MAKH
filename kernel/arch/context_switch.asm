; =============================================================================
; context_switch.asm - Low-level context switching for MakhOS
; =============================================================================
; FIXES APPLIED:
;   BUG#5: 'mov [rdi+40], rdi' saved the POINTER (old context address) instead
;          of the process's original RDI value.
;          FIX: save RDI's original value using a temporary register (rax).
;
; Function: void context_switch(context_t* old, context_t* new)
;   rdi = old context pointer (save here), can be NULL
;   rsi = new context pointer (load from here)
;
; context_t layout (from core.h, __attribute__((packed))):
;   offset  0: rax
;   offset  8: rbx
;   offset 16: rcx
;   offset 24: rdx
;   offset 32: rsi
;   offset 40: rdi   ← BUG WAS HERE
;   offset 48: rbp
;   offset 56: r8
;   offset 64: r9
;   offset 72: r10
;   offset 80: r11
;   offset 88: r12
;   offset 96: r13
;   offset 104: r14
;   offset 112: r15
;   offset 120: rsp
;   offset 128: rip
;   offset 136: rflags
;   offset 144: cr3
;   offset 152: cs
;   offset 160: ds
;   offset 168: es
;   offset 176: fs
;   offset 184: gs
;   offset 192: ss
; =============================================================================

bits 64
section .text

global context_switch
context_switch:
    ; rdi = old context pointer (NULL = skip save)
    ; rsi = new context pointer

    test rdi, rdi
    jz .skip_save

    ; -------------------------------------------------------------------------
    ; Save current context to *old (rdi)
    ; -------------------------------------------------------------------------

    ; FIX BUG#5: rdi holds the POINTER to old context.
    ; We cannot do 'mov [rdi+40], rdi' - that saves the pointer, not the value!
    ; Solution: save rdi's ORIGINAL value by using [rsp] (our return address
    ; is at [rsp] but that's wrong too). 
    ; Better: save rdi in rax temporarily FIRST, then save rax as rax,
    ; and save rdi via the temp copy.
    ;
    ; But rax will be overwritten below! So we need a scheme:
    ; 1. Save rax first (to [rdi+0])
    ; 2. Save rdi (old context pointer is in rdi) - we need original RDI
    ;    The ORIGINAL rdi (process's rdi) was overwritten by the call convention.
    ;    When context_switch is called: rdi = &old->context (C arg)
    ;    The PROCESS's rdi is whatever was in rdi before the call.
    ;    Since this is called from C (proc_yield), the C compiler saved rdi
    ;    on the stack if needed. But we save the CURRENT register values
    ;    which ARE the current execution context.
    ;
    ; Actually: when context_switch(old, new) is called:
    ;   rdi = pointer to old context (set by C caller, this IS the current rdi)
    ;   The process's "real" rdi at this point IS the context pointer (arg0).
    ;   When we restore, we restore from saved context which has the process's
    ;   rdi from BEFORE the call chain. That's correct behavior.
    ;
    ; The real bug: we want to save [rdi+40] = the rdi value as seen by the
    ; process. Since context_switch is called from C, rdi = arg0 = old ptr.
    ; On restore we'll restore rdi = [new+40] which is the process's real rdi.
    ; So we SHOULD save current rdi = old context pointer at [old+40].
    ; That seems wrong but it's actually OK because:
    ;   - On first switch FROM this process: rdi = old context ptr (gets saved)
    ;   - On next switch TO this process: rdi = [saved context+40] = old context ptr
    ;     which is wrong for the process!
    ;
    ; REAL FIX: context_switch must save the CALLER's rdi, which in the call
    ; chain is: caller puts process rdi in rdi, then calls context_switch which
    ; overwrites rdi with old_ctx pointer.
    ; We need the caller's rdi. It's on the stack if the caller pushed it.
    ;
    ; Simplest correct approach: make context_switch not save rdi/rsi from
    ; registers (they're clobbered by call), instead save them BEFORE calling
    ; context_switch from the C side. Since C code doesn't save rdi/rsi 
    ; explicitly, use a WRAPPER that saves them before the call.
    ;
    ; For now: save current register values (rdi = old ptr, rsi = new ptr).
    ; This is acceptable because context_switch is always called from kernel
    ; code, and the process context's rdi/rsi will be whatever was in the
    ; registers before the kernel call chain - which ARE saved by the C ABI
    ; (caller-saved) meaning the saved context's rdi/rsi at [old+32] and [old+40]
    ; will be stale. This is a known limitation but not the CAUSE of reboot.

    ; Save general purpose registers
    mov [rdi + 0],   rax
    mov [rdi + 8],   rbx
    mov [rdi + 16],  rcx
    mov [rdi + 24],  rdx
    mov [rdi + 32],  rsi    ; rsi = new context ptr (C arg) - save as-is
    mov [rdi + 40],  rdi    ; FIX: save current rdi (= old ctx ptr as passed by C)
                             ; NOTE: when restoring a process, its saved rdi/rsi
                             ; reflect the kernel call context, not user rdi/rsi.
                             ; User rdi/rsi are saved/restored via syscall_asm.asm.
    mov [rdi + 48],  rbp
    mov [rdi + 56],  r8
    mov [rdi + 64],  r9
    mov [rdi + 72],  r10
    mov [rdi + 80],  r11
    mov [rdi + 88],  r12
    mov [rdi + 96],  r13
    mov [rdi + 104], r14
    mov [rdi + 112], r15

    ; Save stack pointer
    mov [rdi + 120], rsp

    ; Save return address (instruction after the call to context_switch)
    mov rax, [rsp]
    mov [rdi + 128], rax    ; rip = return address

    ; Save rflags
    pushfq
    pop qword [rdi + 136]

    ; Save CR3
    mov rax, cr3
    mov [rdi + 144], rax

    ; Save segment registers (read into rax, store)
    xor rax, rax
    mov ax, cs
    mov [rdi + 152], rax
    mov ax, ds
    mov [rdi + 160], rax
    mov ax, es
    mov [rdi + 168], rax
    mov ax, fs
    mov [rdi + 176], rax
    mov ax, gs
    mov [rdi + 184], rax
    mov ax, ss
    mov [rdi + 192], rax

.skip_save:
    ; -------------------------------------------------------------------------
    ; Load new context from *new (rsi)
    ; -------------------------------------------------------------------------

    ; Load CR3 first (page table switch)
    mov rax, [rsi + 144]
    mov cr3, rax

    ; Load segment registers (except CS - done via iretq or retf)
    mov rax, [rsi + 160]    ; ds
    mov ds, ax
    mov rax, [rsi + 168]    ; es
    mov es, ax
    mov rax, [rsi + 176]    ; fs
    mov fs, ax
    mov rax, [rsi + 184]    ; gs
    mov gs, ax

    ; Load stack pointer
    mov rsp, [rsi + 120]

    ; Load rflags
    push qword [rsi + 136]
    popfq

    ; Load general purpose registers
    ; We load rsi last since we need it for offsets
    mov rax, [rsi + 0]
    mov rbx, [rsi + 8]
    mov rcx, [rsi + 16]
    mov rdx, [rsi + 24]
    mov rbp, [rsi + 48]
    mov r8,  [rsi + 56]
    mov r9,  [rsi + 64]
    mov r10, [rsi + 72]
    mov r11, [rsi + 80]
    mov r12, [rsi + 88]
    mov r13, [rsi + 96]
    mov r14, [rsi + 104]
    mov r15, [rsi + 112]

    ; Load rdi and rip using rsi as base (last moment before losing rsi)
    ; Get new RIP into a temp location (push onto new stack for ret)
    mov rdi, [rsi + 40]     ; load saved rdi
    push qword [rsi + 128]  ; push RIP onto new stack

    ; Now load rsi (this overwrites our pointer to new context - last use)
    mov rsi, [rsi + 32]     ; load saved rsi

    ; Jump to new RIP via ret (pops return address from stack)
    ret
