; MakhOS Bootloader - Version 0.0.2
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

; GDT location (must be identity mapped and accessible in 32-bit mode)
GDT_ADDR        equ 0x0800         ; GDT at 0x800 (below 1MB, identity mapped)

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
    dd -(MB2_MAGIC + MB2_ARCH + (header_end - multiboot_header)) & 0xFFFFFFFF ; Checksum
    
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
    ; Use register-based absolute addressing (RIP-relative doesn't work for VGA)
    mov edi, VGA_ADDR
    mov dword [edi], 0x0F33      ; '3' white
    mov dword [edi + 2], 0x0F32  ; '2' white
    mov dword [edi + 4], 0x0F2D  ; '-' white
    mov dword [edi + 6], 0x0F62  ; 'b' white
    mov dword [edi + 8], 0x0F69  ; 'i' white
    mov dword [edi + 10], 0x0F74 ; 't' white
    
    ; Save multiboot info pointer on stack
    ; Push as 64-bit value for proper alignment when popped in long mode
    push dword 0                    ; High 32 bits (zero extend)
    push esi                        ; Low 32 bits - now 8 bytes total for 64-bit pop
    
    ; ==========================================================================
    ; COPY GDT TO LOW MEMORY (identity mapped location)
    ; ==========================================================================
    ; The GDT needs to be at a known address that's identity mapped.
    ; We copy it to 0x800 which is below 1MB and identity mapped.
    ;
    ; Source: gdt64_data (in the kernel at 0x100000+)
    ; Destination: GDT_ADDR (0x800)
    ; ==========================================================================
    call .get_eip_for_gdt
.get_eip_for_gdt:
    pop ebx                             ; EBX = current EIP
    lea esi, [ebx + gdt64_data - .get_eip_for_gdt]  ; ESI = source (gdt64_data)
    mov edi, GDT_ADDR                   ; EDI = destination (0x800)
    mov ecx, gdt64_end - gdt64_data     ; ECX = size
    rep movsb                           ; Copy GDT to low memory
    
    ; Setup GDT descriptor in low memory
    lea esi, [ebx + gdt64_descriptor_data - .get_eip_for_gdt]
    mov edi, GDT_ADDR + (gdt64_end - gdt64_data)  ; After GDT entries
    mov ecx, 8                          ; 8 bytes for descriptor
    rep movsb
    
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
    ; ENABLE SSE (required for optimized code using XMM registers)
    ; ==========================================================================
    mov eax, cr0
    and ax, 0xFFFB          ; Clear CR0.EM (bit 2)
    or ax, 0x2              ; Set CR0.MP (bit 1)
    mov cr0, eax
    
    mov eax, cr4
    or ax, 0x600            ; Set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10)
    mov cr4, eax
    
    ; Checkpoint: 'X' for SSE enabled
    mov eax, VGA_ADDR + 22
    mov dword [eax], 0x0F58   ; 'X' white
    
    ; ==========================================================================
    ; ENABLE PAE (Physical Address Extension)
    ; ==========================================================================
    mov eax, cr4
    or eax, CR4_PAE
    mov cr4, eax
    
    ; Checkpoint: 'A' for PAE enabled
    mov eax, VGA_ADDR + 24
    mov dword [eax], 0x0F41   ; 'A' white
    
    ; ==========================================================================
    ; ENABLE LONG MODE (set LME bit in EFER MSR)
    ; ==========================================================================
    mov ecx, EFER_MSR
    rdmsr
    or eax, EFER_LME
    wrmsr
    
    ; Checkpoint: 'E' for EFER.LME set
    mov eax, VGA_ADDR + 26
    mov dword [eax], 0x0F45   ; 'E' white
    
    ; ==========================================================================
    ; LOAD CR3 (Page Table Base)
    ; ==========================================================================
    mov eax, PML4_ADDR
    mov cr3, eax                    ; Load PML4 address
    
    ; Checkpoint: '3' for CR3 loaded
    mov eax, VGA_ADDR + 28
    mov dword [eax], 0x0F33   ; '3' white
    
    ; ==========================================================================
    ; LOAD GDT64 (BEFORE enabling paging - critical!)
    ; ==========================================================================
    ; We must load the 64-bit GDT BEFORE enabling paging because once paging
    ; is enabled, the CPU enters compatibility mode and needs a valid GDT.
    ; If we try to far jump with selector 0x08 before loading GDT64,
    ; we get a General Protection Fault since selector 0x08 doesn't exist
    ; in GRUB's 32-bit GDT.
    ;
    ; GDT is now at GDT_ADDR (0x800) which is identity mapped.
    ; Descriptor is at GDT_ADDR + (gdt64_end - gdt64_data)
    ; ==========================================================================
    ; Use absolute addressing for LGDT (not RIP-relative)
    ; We need to load the descriptor from physical address 0x818
    mov eax, GDT_ADDR + (gdt64_end - gdt64_data)
    lgdt [eax]
    
    ; Checkpoint: 'G' for GDT loaded
    mov eax, VGA_ADDR + 30
    mov dword [eax], 0x0F47   ; 'G' white
    
    ; ==========================================================================
    ; ENABLE PAGING (this activates long mode)
    ; ==========================================================================
    mov eax, cr0
    or eax, CR0_PG
    mov cr0, eax                    ; Enable paging - we're now in compatibility mode with GDT64!
    
    ; Checkpoint: 'P' for Paging enabled
    mov eax, VGA_ADDR + 32
    mov dword [eax], 0x0F50   ; 'P' white
    
    ; ==========================================================================
    ; FAR JUMP TO 64-BIT CODE
    ; ==========================================================================
    ; Use retf (far return) trick to perform far jump with runtime-calculated address
    push CODE_SEG64                 ; CS = 0x08 (64-bit code segment)
    call .get_eip_for_jump
.get_eip_for_jump:
    pop ebx
    lea eax, [ebx + long_mode_start - .get_eip_for_jump]
    push eax                        ; EIP = absolute address of long_mode_start
    retf                            ; Far return to 64-bit code

; ============================================================================
; 64-BIT GDT DATA (copied to low memory at runtime)
; ============================================================================
align 8
gdt64_data:
    dq 0                            ; Null descriptor (0x00)

gdt64_code:                         ; 64-bit Code Segment (0x08)
    dw 0xFFFF                       ; Limit (ignored)
    dw 0                            ; Base (ignored)
    db 0
    db 10011010b                    ; Present, Ring 0, Code, Executable, Readable
    db 10101111b                    ; Long mode, 4KB gran
    db 0

gdt64_data_entry:                   ; 64-bit Data Segment (0x10)
    dw 0xFFFF
    dw 0
    db 0
    db 10010010b                    ; Present, Ring 0, Data, Writable
    db 00000000b
    db 0

gdt64_end:

; GDT Descriptor data (also copied to low memory)
gdt64_descriptor_data:
    dw gdt64_end - gdt64_data - 1   ; Size (16-bit)
    dd GDT_ADDR                     ; Address (0x800 - identity mapped)

; ============================================================================
; 64-BIT LONG MODE CODE
; ============================================================================
bits 64

long_mode_start:
    ; Now in 64-bit long mode!
    
    ; Immediate checkpoint: 'L' for Long mode entered
    mov rcx, VGA_ADDR + 320         ; Third line
    mov ah, VGA_GREEN
    mov al, 'L'
    mov [rcx], ax
    
    ; Setup data segments
    mov ax, DATA_SEG64
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Checkpoint: 'D' for Data segments done
    mov al, 'D'
    mov [rcx + 2], ax
    
    ; Restore multiboot info pointer
    pop rdi                         ; RDI = multiboot2 info structure (first arg for C)
    
    ; Store multiboot info pointer in global variable for C code
    mov [multiboot_info_ptr], rdi
    
    ; Checkpoint: 'P' for Pop done
    mov al, 'P'
    mov [rcx + 4], ax
    
    ; Setup 64-bit stack
    mov rsp, STACK_TOP
    
    ; Checkpoint: 'S' for Stack setup
    mov al, 'S'
    mov [rcx + 6], ax
    
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
    
    ; Checkpoint: 'K' for Kernel call next
    mov al, 'K'
    mov [rcx + 12], ax
    
    ; Additional checkpoint before kernel_main - 'C' for Call
    mov al, 'C'
    mov [rcx + 14], ax
    
    ; Jump to kernel main
    call kernel_main
    
    ; Checkpoint if we return (should not happen) - 'R' for Return
    mov al, 'R'
    mov [rcx + 16], ax
    
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
; GLOBAL SYMBOLS (exported to C code)
; ============================================================================
global multiboot_info_ptr

; ============================================================================
; DATA SECTION
; ============================================================================
section .data
align 8
multiboot_info_ptr:
    dq 0                            ; Will be set to RDI value in long_mode_start

; ============================================================================
; BSS SECTION (uninitialized data)
; ============================================================================
section .bss
align 16

kernel_stack_bottom:
    resb 65536                      ; 64KB stack
kernel_stack_top:
