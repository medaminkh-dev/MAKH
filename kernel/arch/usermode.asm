; =============================================================================
; usermode.asm - Transition to user mode (ring 3) via iretq
; =============================================================================
;
; void enter_usermode(uint64_t entry, uint64_t stack, uint64_t arg)
;   rdi = entry point (user virtual address)
;   rsi = user stack pointer (top of user stack, 16-byte aligned)
;   rdx = argument to pass in rdi to user program
;
; iretq frame layout (from LOW to HIGH address, top = rsp):
;   [rsp + 0]  = RIP     (return address = entry point)
;   [rsp + 8]  = CS      (user code segment = 0x1B)
;   [rsp + 16] = RFLAGS  (must have IF=1 to enable interrupts)
;   [rsp + 24] = RSP     (user stack pointer)
;   [rsp + 32] = SS      (user data segment = 0x23)
;
; After iretq:
;   - CPU loads RIP from [rsp+0]
;   - CPU loads CS  from [rsp+8]  -> switches to ring 3
;   - CPU loads RFLAGS
;   - CPU loads RSP from [rsp+24] -> switches to user stack
;   - CPU loads SS  from [rsp+32]
; =============================================================================

bits 64
section .text

USER_CS equ 0x1B    ; GDT entry 3 (0x18) | RPL 3
USER_DS equ 0x23    ; GDT entry 4 (0x20) | RPL 3
USER_IF equ 0x0200  ; RFLAGS.IF = 1 (interrupts enabled)

global enter_usermode
enter_usermode:
    ; rdi = entry point
    ; rsi = user stack pointer
    ; rdx = argument for user program

    ; Step 1: Load user segment registers
    ; We must set DS/ES/FS/GS to user data segment BEFORE iretq
    ; so the user program starts with correct segments.
    ; SS will be set by iretq.
    mov ax, USER_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Step 2: Build iretq frame on CURRENT (kernel) stack
    ; We use sub rsp + direct mov to avoid any confusion about offsets.
    ;
    ; Save entry and stack pointer (we'll need them for the frame)
    ; rdi = entry, rsi = user_stack, rdx = arg
    ; We save entry and stack in preserved registers temporarily,
    ; then build the frame cleanly.
    ;
    ; r10 = entry point (save rdi)
    ; r11 = user stack
    ; r12 = arg
    mov r10, rdi    ; r10 = entry point
    mov r11, rsi    ; r11 = user stack pointer
    mov r12, rdx    ; r12 = argument

    ; Step 3: Build iretq frame (5 * 8 = 40 bytes)
    ; Push in REVERSE order (SS first since stack grows down)
    ; After all pushes, [rsp] = RIP at top

    push qword USER_DS      ; SS  (pushed first = highest addr)
    push r11                ; RSP (user stack pointer)
    pushfq                  ; push current RFLAGS
    pop rax
    or rax, USER_IF         ; set IF=1 (enable interrupts in user mode)
    push rax                ; RFLAGS
    push qword USER_CS      ; CS
    push r10                ; RIP (entry point)

    ; At this point iretq frame is:
    ;   [rsp+0]  = RIP  = entry point
    ;   [rsp+8]  = CS   = 0x1B
    ;   [rsp+16] = RFLAGS with IF=1
    ;   [rsp+24] = RSP  = user stack
    ;   [rsp+32] = SS   = 0x23

    ; Step 4: Set argument for user program
    ; User program will receive the argument in RDI (first argument register)
    mov rdi, r12    ; rdi = arg

    ; Step 5: Jump to user mode
    iretq
