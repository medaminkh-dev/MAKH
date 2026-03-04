; idt_asm.asm - Interrupt stubs for x86_64
; NASM syntax

bits 64
section .text

; External C handler
extern exception_handler

; Default stub for undefined ISRs
isr_default:
    push 0xDEADBEEF         ; dummy error code
    push 0xFF               ; default vector number
    jmp isr_common

; Macro for interrupts WITHOUT error code
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0                  ; dummy error code
    push %1                 ; interrupt number
    jmp isr_common
%endmacro

; Macro for interrupts WITH error code
%macro ISR_ERR 1
global isr%1
isr%1:
    push %1                 ; interrupt number
    jmp isr_common
%endmacro

; Common interrupt handler
isr_common:
    ; Save all general purpose registers
    push rax
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
    push r15
    
    ; Call C handler
    mov rdi, rsp            ; rdi = pointer to registers_t
    call exception_handler
    
    ; Restore registers
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
    pop rax
    
    ; Clean up error code and interrupt number
    add rsp, 16
    iretq

; CPU Exceptions (0-31)
ISR_NOERR 0   ; Divide by Zero
ISR_NOERR 1   ; Debug
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; Breakpoint
ISR_NOERR 4   ; Overflow
ISR_NOERR 5   ; Bound Range
ISR_NOERR 6   ; Invalid Opcode
ISR_NOERR 7   ; Device Not Available
ISR_ERR   8   ; Double Fault
ISR_NOERR 9   ; Coprocessor Segment Overrun
ISR_ERR   10  ; Invalid TSS
ISR_ERR   11  ; Segment Not Present
ISR_ERR   12  ; Stack Fault
ISR_ERR   13  ; General Protection Fault
ISR_ERR   14  ; Page Fault
ISR_NOERR 15  ; Reserved
ISR_NOERR 16  ; x87 FPU
ISR_ERR   17  ; Alignment Check
ISR_NOERR 18  ; Machine Check
ISR_NOERR 19  ; SIMD FPU
ISR_NOERR 20  ; Virtualization
ISR_ERR   21  ; Control Protection
ISR_NOERR 22  ; Reserved
ISR_NOERR 23  ; Reserved
ISR_NOERR 24  ; Reserved
ISR_NOERR 25  ; Reserved
ISR_NOERR 26  ; Reserved
ISR_NOERR 27  ; Reserved
ISR_NOERR 28  ; Reserved
ISR_NOERR 29  ; Reserved
ISR_ERR   30  ; Security Exception
ISR_NOERR 31  ; Reserved

; Test vectors
ISR_NOERR 0x30  ; Test vector 48 (0x30)
ISR_NOERR 0x48  ; Test vector 72 (0x48)

; Create stub table for C
; Only define entries for ISRs that exist, others get default handler
section .data
global isr_stub_table
isr_stub_table:
    ; CPU Exceptions 0-31
    dq isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    dq isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    
    ; Vectors 32-47 (reserved/IRQs) - use default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    
    ; Vectors 48-71 - fill with defaults
    dq isr0x30  ; Custom test handler at vector 0x30 (48)
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    dq isr_default, isr_default, isr_default, isr_default
    
    ; Vector 0x48 (72) - second test handler
    dq isr0x48
    
    ; Remaining vectors
%rep 183
    dq isr_default
%endrep
