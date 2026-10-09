; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 Amine Khemissi
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
PDPT_ADDR       equ 0x2000         ; Page Directory Pointer Table at 0x2000 (low identity)
PD_ADDR         equ 0x3000         ; Page Directory at 0x3000 (low identity)
PDPT_HI_ADDR    equ 0x4000         ; PDPT for the higher-half kernel window
PD_HI_ADDR      equ 0x5000         ; PD for the higher-half kernel window
PDPT_HHDM_ADDR  equ 0x6000         ; PDPT for the higher-half direct map
PD_HHDM_ADDR    equ 0x7000         ; PD for the higher-half direct map

; Higher-half kernel link base (must match linker.ld KERNEL_VMA and
; mm/vmm.h KERNEL_VMA_BASE). 0xFFFFFFFF80000000 is PML4 slot 511, PDPT slot 510.
KERNEL_VMA      equ 0xFFFFFFFF80000000
PML4_HI_SLOT    equ 511            ; (KERNEL_VMA >> 39) & 0x1FF
PDPT_HI_SLOT    equ 510            ; (KERNEL_VMA >> 30) & 0x1FF

; Higher-half direct map base (must match mm/vmm.h HHDM_BASE). 0xFFFF800000000000
; is PML4 slot 256, PDPT slot 0. Set up early so VGA (reached via the HHDM, so it
; works under a user CR3 that has no low identity map) is live from kernel_main.
HHDM_VMA        equ 0xFFFF800000000000
PML4_HHDM_SLOT  equ 256            ; (HHDM_VMA >> 39) & 0x1FF

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

    ; Framebuffer request tag: ask GRUB to set a 1024x768x32 linear-RGB graphics
    ; mode and pass its address back in the framebuffer info tag. flags bit0 = 1
    ; (optional), so if the firmware can't provide it the boot still proceeds and
    ; the kernel falls back to VGA text mode (fb_init() returns 0). This is what
    ; lets MAKH render the pixel console and the fennec boot splash.
align 8
fb_request_start:
    dw 5                            ; Type: framebuffer
    dw 1                            ; Flags: optional
    dd fb_request_end - fb_request_start ; Size
    dd 1024                         ; Requested width
    dd 768                          ; Requested height
    dd 32                           ; Requested depth (bits per pixel)
fb_request_end:

    ; End tag (required)
align 8
    dw 0                            ; Type: end
    dw 0                            ; Flags
    dd 8                            ; Size
header_end:

; ============================================================================
; 32-BIT BOOT CODE (GRUB starts us here in protected mode)
; Linked 1:1 at its physical load address (low .boot section): it runs with
; paging off and during the switch to long mode, before the jump into the
; higher half.
; ============================================================================
section .boot progbits alloc exec nowrite align=16
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
    
    ; Clear page tables (PML4, low PDPT/PD, high PDPT/PD, HHDM PDPT/PD = 7 pages)
    mov edi, PML4_ADDR
    xor eax, eax
    mov ecx, 4096 * 7               ; Clear 7 pages (28KB)
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
    ; HIGHER-HALF KERNEL WINDOW
    ; Map KERNEL_VMA (0xFFFFFFFF80000000) -> physical 0 so the kernel, linked in
    ; the -2GB region (-mcmodel=kernel), can execute once paging is on.
    ;   PML4[511] -> PDPT_HI ; PDPT_HI[510] -> PD_HI ; PD_HI[0..31] -> 0..64MB.
    ; ==========================================================================
    mov edi, PML4_ADDR + PML4_HI_SLOT * 8
    mov eax, PDPT_HI_ADDR | 0x03
    mov [edi], eax

    mov edi, PDPT_HI_ADDR + PDPT_HI_SLOT * 8
    mov eax, PD_HI_ADDR | 0x03
    mov [edi], eax

    ; 32 huge pages = 64MB high, covering the kernel image with headroom.
    mov edi, PD_HI_ADDR
    mov eax, 0x83                   ; phys 0, Present + Writable + Huge
    mov ecx, 32
.fill_hi_pd:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_hi_pd

    ; ==========================================================================
    ; HIGHER-HALF DIRECT MAP (early)
    ; Map HHDM_VMA (0xFFFF800000000000) -> physical 0 so the kernel can reach low
    ; physical through the higher half before vmm_init builds the full map. VGA
    ; lives here (0xFFFF8000000B8000) — that is how the write() syscall path
    ; reaches the screen under a user CR3 once the low identity map is dropped.
    ;   PML4[256] -> PDPT_HHDM ; PDPT_HHDM[0] -> PD_HHDM ; PD_HHDM[0..31] -> 0..64MB
    ; vmm_init later replaces slot 256 with the full-RAM direct map.
    ; ==========================================================================
    mov edi, PML4_ADDR + PML4_HHDM_SLOT * 8
    mov eax, PDPT_HHDM_ADDR | 0x03
    mov [edi], eax

    mov edi, PDPT_HHDM_ADDR          ; PDPT slot 0
    mov eax, PD_HHDM_ADDR | 0x03
    mov [edi], eax

    mov edi, PD_HHDM_ADDR
    mov eax, 0x83                    ; phys 0, Present + Writable + Huge
    mov ecx, 32
.fill_hhdm_pd:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_hhdm_pd

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
    
    ; Restore multiboot info pointer. Keep it in RDI across the jump into the
    ; higher half; the high stub stores it once the kernel's .data is reachable.
    pop rdi                         ; RDI = multiboot2 info structure

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
    
    ; Additional checkpoint before the jump - 'H' for Higher-half
    mov al, 'H'
    mov [rcx + 14], ax

    ; Jump into the higher half. kernel_high_start lives in .text at KERNEL_VMA+,
    ; which is mapped (PML4[511]); an absolute 64-bit jump leaves this low .boot
    ; trampoline behind for good. RDI (multiboot info) is preserved.
    mov rax, kernel_high_start
    jmp rax

    ; Should never reach here, but halt if we do
.halt:
    cli
    hlt
    jmp .halt

; ============================================================================
; HIGHER-HALF ENTRY
; Linked in .text at KERNEL_VMA+ (-mcmodel=kernel). The low trampoline jumps
; here with RDI = multiboot info; by now the kernel's own .data/.text are the
; addresses we execute from, so we can finish setup and enter C.
; ============================================================================
section .text
bits 64
global kernel_high_start
extern kernel_main
kernel_high_start:
    ; Switch off the low boot stack (0x90000, only in the identity map) onto the
    ; kernel's own stack in .bss (higher half, PML4 slot 511 — shared into every
    ; address space). This is the initial/idle thread's stack, so it must stay
    ; mapped even under a user CR3 that no longer carries the low identity map.
    lea rsp, [rel kernel_stack_top]
    mov [rel multiboot_info_ptr], rdi   ; stash for the C side (kernel.c)
    call kernel_main                    ; never returns
.hang:
    cli
    hlt
    jmp .hang

; ============================================================================
; GLOBAL SYMBOLS (exported to C code)
; ============================================================================
global multiboot_info_ptr

; ============================================================================
; DATA SECTION (higher half: collected into the kernel's .data by linker.ld)
; ============================================================================
section .data
align 8
multiboot_info_ptr:
    dq 0                            ; set by kernel_high_start from RDI

; ============================================================================
; BSS SECTION (uninitialized data)
; ============================================================================
section .bss
align 16

kernel_stack_bottom:
    resb 65536                      ; 64KB stack
kernel_stack_top:
