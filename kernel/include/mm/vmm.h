/**
 * MakhOS - vmm.h
 * Virtual Memory Manager header
 * Implements 4-level paging (PML4 -> PDPT -> PD -> PT) for x86_64
 */

#ifndef MAKHOS_VMM_H
#define MAKHOS_VMM_H

#include <types.h>

/* Page flags (Intel x86_64 format) */
#define PAGE_PRESENT      (1 << 0)
#define PAGE_WRITABLE     (1 << 1)
#define PAGE_USER         (1 << 2)
#define PAGE_WRITETHROUGH (1 << 3)
#define PAGE_CACHE_DISABLE (1 << 4)
#define PAGE_ACCESSED     (1 << 5)
#define PAGE_DIRTY        (1 << 6)
#define PAGE_HUGE         (1 << 7)  /* 2MB or 1GB pages */
#define PAGE_GLOBAL       (1 << 8)
#define PAGE_NO_EXECUTE   (1ULL << 63)  /* NX bit (requires IA32_EFER.NXE) */

/* Virtual address indices extraction macros */
/* PML4 index: bits 47-39 */
#define VMM_PML4_INDEX(virt) (((virt) >> 39) & 0x1FF)
/* PDPT index: bits 38-30 */
#define VMM_PDPT_INDEX(virt) (((virt) >> 30) & 0x1FF)
/* PD index: bits 29-21 */
#define VMM_PD_INDEX(virt)   (((virt) >> 21) & 0x1FF)
/* PT index: bits 20-12 */
#define VMM_PT_INDEX(virt)   (((virt) >> 12) & 0x1FF)
/* Page offset: bits 11-0 */
#define VMM_PAGE_OFFSET(virt) ((virt) & 0xFFF)

/* Higher half kernel mapping base */
#define KERNEL_HIGHER_HALF_BASE 0xFFFF800000000000ULL

/* Recursive mapping in last PML4 entry */
#define RECURSIVE_PML4_INDEX 511

/* Number of entries per page table */
#define PAGE_TABLE_ENTRIES 512

/* Initialize VMM */
void vmm_init(void);

/* Create new page tables (for new process) */
uint64_t vmm_create_address_space(void);

/* Switch address space (change CR3) */
void vmm_switch_address_space(uint64_t pml4_phys);

/* Map physical address to virtual address */
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);

/* Unmap virtual page */
int vmm_unmap_page(uint64_t virt_addr);

/* Get physical address for virtual address */
uint64_t vmm_get_physical(uint64_t virt_addr);

/* Allocate virtual page (with physical page from PMM) */
void* vmm_alloc_page(uint64_t flags);

/* Free virtual page */
void vmm_free_page(void* virt_addr);

/* Assembly functions (from paging_asm.asm) */
extern void vmm_load_pml4(uint64_t pml4_phys);
extern void vmm_enable_paging(void);
extern uint64_t vmm_get_cr3(void);
extern void vmm_invlpg(uint64_t virt_addr);

#endif /* MAKHOS_VMM_H */
