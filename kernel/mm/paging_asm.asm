; MakhOS - paging_asm.asm
; Assembly helpers for Virtual Memory Manager
; Implements x86_64 paging operations

bits 64

; Global exports
global vmm_load_pml4
global vmm_enable_paging
global vmm_get_cr3
global vmm_invlpg

section .text

; Load CR3 with new PML4 address
; rdi = physical address of PML4 (System V AMD64 ABI)
vmm_load_pml4:
    mov rax, rdi
    mov cr3, rax
    ret

; Enable paging (PAE, long mode, PG bit)
; Note: PAE and long mode should already be enabled by boot.asm
; This function ensures paging is fully enabled
vmm_enable_paging:
    ; Enable PAE (Physical Address Extension) in CR4
    mov rax, cr4
    or rax, (1 << 5)  ; CR4.PAE = 1
    mov cr4, rax
    
    ; Enable long mode in EFER MSR
    mov ecx, 0xC0000080  ; IA32_EFER MSR
    rdmsr
    or eax, (1 << 8)     ; EFER.LME = 1 (Long Mode Enable)
    or eax, (1 << 11)    ; EFER.NXE = 1 (NX bit support)
    wrmsr
    
    ; Enable paging in CR0
    mov rax, cr0
    or rax, 0x80000000   ; CR0.PG = 1 (Paging Enable)
    mov cr0, rax
    
    ret

; Get current CR3 value
; Returns: rax = current CR3 (physical address of PML4)
vmm_get_cr3:
    mov rax, cr3
    ret

; Invalidate TLB entry for specific page
; rdi = virtual address to invalidate
vmm_invlpg:
    invlpg [rdi]
    ret
