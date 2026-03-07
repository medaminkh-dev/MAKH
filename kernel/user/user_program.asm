; =============================================================================
; user_program.S - User Mode Program with Syscalls
; =============================================================================

bits 64
section .user_text

global user_program_start

user_program_start:
    ; Set segment registers for user mode
    mov ax, 0x23        ; USER_DS = 0x20 | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    ; Print "Hello from user mode!"
    lea rsi, [rel msg_hello]
    mov rdx, 23
    mov rax, 1
    mov rdi, 1
    syscall
    
    ; Loop counter - count from 0 to 9
    xor r15, r15                 ; r15 = 0
    
.loop:
    ; Save r15 (syscall might clobber it)
    push r15
    
    ; Print "Count: "
    lea rsi, [rel msg_count]
    mov rdx, 7
    mov rax, 1
    mov rdi, 1
    syscall
    
    ; Print digit
    pop r15
    push r15                      ; Save again
    lea rsi, [rel digits]
    add rsi, r15
    mov rdx, 1
    mov rax, 1
    mov rdi, 1
    syscall
    
    ; Print newline
    lea rsi, [rel newline_msg]
    mov rdx, 1
    mov rax, 1
    mov rdi, 1
    syscall
    
    ; Short delay
    mov r14, 100000
.delay:
    dec r14
    jnz .delay
    
    ; Increment counter
    pop r15
    inc r15
    cmp r15, 10
    jl .loop
    
    ; Exit
    xor rax, rax
    xor rdi, rdi
    syscall
    
    jmp $

; Data
msg_hello:
    db "Hello from user mode!", 10
    
msg_count:
    db "Count: "
    
digits:
    db "0123456789"
    
newline_msg:
    db 10

global user_program_end
user_program_end:
