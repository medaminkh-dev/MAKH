; MakhOS Bootloader - Phase 1
; boot.asm - Multiboot2 compliant bootloader with 32→64 bit transition
; Target: x86_64 (amd64)

; ============================================================================
; CONSTANTS
; ============================================================================

; Multiboot2 Header Constants
MB2_MAGIC       equ 0xe85250d6    ; Multiboot2 magic number
MB2_ARCH        equ 0              ; i386 protected mode architecture

; Page table constants
PML4_ADDR       equ 0x1000         ; PML4 at 0x1000
PDPT_ADDR       equ 0x2000         ; Page Directory Pointer Table at 0x2000
PD_ADDR         equ 0x3000         ; Page Directory at 0x3000

; VGA constants
VGA_ADDR        equ 0xB8000        ; VGA text buffer address
VGA_GREEN       equ 0x0A           ; Green on black
VGA_WHITE       equ 0x0F           ; White on black
VGA_RED         equ 0x0C           ; Red on black

; GDT constants
CODE_SEG64      equ 0x08           ; 64-bit code segment selector
DATA_SEG64      equ 0x10           ; 64-bit data segment selector

; MSR constants
EFER_MSR        equ 0xC0000080     ; Extended Feature Enable Register
EFER_LME        equ (1 << 8)       ; Long Mode Enable bit

; CR0 constants
CR0_PE          equ (1 << 0)       ; Protected Mode Enable
CR0_PG          equ (1 << 31)      ; Paging Enable

; CR4 constants
CR4_PAE         equ (1 << 5)       ; Physical Address Extension

; Stack location
STACK_TOP       equ 0x90000        ; Stack grows downward from here

; ============================================================================
; MULTIBOOT2 HEADER (must be within first 32768 bytes and aligned to 8 bytes)
; ============================================================================
section .multiboot
align 8

multiboot_header:
    dd MB2_MAGIC                    ; Magic number
    dd MB2_ARCH                     ; Architecture (i386 protected mode)
    dd header_end - multiboot_header ; Header length
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - multiboot_header)) ; Checksum
    
    ; End tag (required)
    dw 0                            ; Type: end
    dw 0                            ; Flags
    dd 8                            ; Size
header_end:

; ============================================================================
; 32-BIT BOOT CODE (GRUB starts us here in protected mode)
; ============================================================================
section .text
bits 32

global start_32bit
start_32bit:
    ; GRUB gives us:
    ; - Protected mode (32-bit)
    ; - EAX = 0x36d76289 (multiboot2 magic)
    ; - EBX = pointer to multiboot2 information structure
    ; - CS = 32-bit code segment
    ; - DS, ES, FS, GS, SS = 32-bit data segments
    
    ; Save multiboot2 info pointer for later
    mov esi, ebx
    
    ; Disable interrupts (we're about to change segments)
    cli
    
    ; Setup stack
    mov esp, STACK_TOP
    
    ; Checkpoint 1: Print stage message (32-bit)
    mov edi, VGA_ADDR
    mov ah, VGA_WHITE
    mov al, '3'
    mov [edi], ax
    mov al, '2'
    mov [edi + 2], ax
    mov al, '-'
    mov [edi + 4], ax
    mov al, 'b'
    mov [edi + 6], ax
    mov al, 'i'
    mov [edi + 8], ax
    mov al, 't'
    mov [edi + 10], ax
    
    ; Save multiboot info pointer on stack
    push esi
    
    ; ==========================================================================
    ; SETUP PAGE TABLES FOR IDENTITY MAPPING (first 2MB using huge pages)
    ; ==========================================================================
    
    ; Clear page tables
    mov edi, PML4_ADDR
    xor eax, eax
    mov ecx, 4096 * 3               ; Clear 3 pages (12KB)
    rep stosb
    
    ; Setup PML4 (Level 4) - point to PDPT
    mov edi, PML4_ADDR
    mov eax, PDPT_ADDR | 0x03       ; Present + Writable
    mov [edi], eax
    
    ; Setup PDPT (Level 3) - point to PD
    mov edi, PDPT_ADDR
    mov eax, PD_ADDR | 0x03         ; Present + Writable
    mov [edi], eax
    
    ; Setup PD (Level 2) - use 2MB huge pages
    mov edi, PD_ADDR
    mov eax, 0x83                   ; Present + Writable + Huge (2MB page)
    mov [edi], eax                  ; Map first 2MB
    mov eax, 0x200083               ; Next 2MB at 0x200000
    mov [edi + 8], eax
    
    ; ==========================================================================
    ; ENABLE PAE (Physical Address Extension)
    ; ==========================================================================
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax
    
    ; ==========================================================================
    ; ENABLE LONG MODE (set LME bit in EFER MSR)
    ; ==========================================================================
    mov ecx, EFER_MSR
    rdmsr
    or eax, EFER_LME
    wrmsr
    
    ; ==========================================================================
    ; ENABLE PAGING (this activates long mode)
    ; ==========================================================================
    mov eax, PML4_ADDR
    mov cr3, eax                    ; Load PML4 address
    
    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax                    ; Enable paging - we're now in compatibility mode!
    
    ; ==========================================================================
    ; SETUP 64-BIT GDT AND JUMP TO LONG MODE
    ; ==========================================================================
    
    ; Load GDT
    lgdt [gdt64_descriptor]
    
    ; Far jump to 64-bit code
    jmp CODE_SEG64:long_mode_start

; ============================================================================
; 64-BIT GDT (Flat model for long mode)
; ============================================================================
align 8
gdt64:
    dq 0                            ; Null descriptor

.gdt64_code:                        ; 64-bit code segment
    dw 0xFFFF                       ; Limit (ignored in long mode)
    dw 0                            ; Base (ignored)
    db 0
    db 10011010b                    ; Present, Ring 0, Code, Executable, Readable
    db 10101111b                    ; Long mode, 32-bit operand size (required), 4KB gran
    db 0

.gdt64_data:                        ; 64-bit data segment
    dw 0xFFFF
    dw 0
    db 0
    db 10010010b                    ; Present, Ring 0, Data, Writable
    db 00000000b
    db 0

gdt64_end:

gdt64_descriptor:
    dw gdt64_end - gdt64 - 1        ; Size
    dq gdt64                        ; Address

; ============================================================================
; 64-BIT LONG MODE CODE
; ============================================================================
bits 64

long_mode_start:
    ; Now in 64-bit long mode!
    
    ; Setup data segments
    mov ax, DATA_SEG64
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Restore multiboot info pointer
    pop rdi                         ; RDI = multiboot2 info structure (first arg for C)
    
    ; Setup 64-bit stack
    mov rsp, STACK_TOP
    
    ; Checkpoint 2: Print stage message (64-bit) - Second line
    mov rcx, VGA_ADDR + 160         ; Second line (160 bytes offset)
    mov ah, VGA_GREEN
    mov al, '6'
    mov [rcx], ax
    mov al, '4'
    mov [rcx + 2], ax
    mov al, '-'
    mov [rcx + 4], ax
    mov al, 'b'
    mov [rcx + 6], ax
    mov al, 'i'
    mov [rcx + 8], ax
    mov al, 't'
    mov [rcx + 10], ax
    
    ; Jump to kernel main
    call kernel_main
    
    ; Should never reach here, but halt if we do
.halt:
    cli
    hlt
    jmp .halt

; ============================================================================
; EXTERNAL SYMBOLS (defined in C code)
; ============================================================================
extern kernel_main

; ============================================================================
; BSS SECTION (uninitialized data)
; ============================================================================
section .bss
align 16

kernel_stack_bottom:
    resb 65536                      ; 64KB stack
kernel_stack_top:
