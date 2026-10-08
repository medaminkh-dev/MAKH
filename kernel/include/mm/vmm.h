/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
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
#define PAGE_COW          (1ULL << 9)   /* software bit: copy-on-write (Phase 17) */

/* 40-bit physical frame field of a PTE (clears flags, NX and reserved bits). */
#define PTE_PHYS_MASK     0x000FFFFFFFFFF000ULL

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

/*
 * Higher-half direct map (HHDM). Physical frame P is always reachable at
 * virtual HHDM_BASE + P. This lives in the higher half (PML4 slot 256) so that
 * the kernel can edit any page table / touch any frame no matter which address
 * space's CR3 is live — the groundwork for a higher-half kernel that leaves the
 * whole low canonical half to user programs (F21 Path A). For now it coexists
 * with the legacy low identity map; code migrates onto it brick by brick.
 */
#define HHDM_BASE KERNEL_HIGHER_HALF_BASE

/* phys -> kernel pointer through the HHDM; V2P is its inverse for HHDM ptrs. */
static inline void*    P2V(uint64_t phys)  { return (void*)(uintptr_t)(phys + HHDM_BASE); }
static inline uint64_t V2P(const void* virt){ return (uint64_t)(uintptr_t)virt - HHDM_BASE; }

/* Base for vmm_alloc_page()'s bump allocations (slot 257, just above the HHDM
 * so a 1 GiB direct map never collides with it). */
#define VMM_ALLOC_BASE 0xFFFF808000000000ULL

/*
 * Higher-half kernel link base. The kernel is linked with -mcmodel=kernel at
 * KERNEL_VMA_BASE + physical (PML4 slot 511), so a kernel symbol's address is
 * its physical load address plus this base. KV2P() recovers the physical — for
 * CR3 and for the HHDM page-table entries that point at kernel-static tables.
 * This is distinct from the HHDM's V2P(): KV2P undoes the -2 GiB kernel link
 * bias, V2P undoes the direct-map bias. (Before the high-relink brick the
 * kernel is identity-mapped low and KERNEL_VMA_BASE is effectively 0.)
 */
#define KERNEL_VMA_BASE 0xFFFFFFFF80000000ULL
static inline uint64_t KV2P(const void* v) {
    return (uint64_t)(uintptr_t)v - KERNEL_VMA_BASE;
}

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

/* Map a ring-3-accessible page (sets PAGE_USER on every level of the walk). */
int vmm_map_user_page(uint64_t va, uint64_t phys, uint64_t flags);

/* Unmap virtual page */
int vmm_unmap_page(uint64_t virt_addr);

/* Get physical address for virtual address */
uint64_t vmm_get_physical(uint64_t virt_addr);

/* Allocate virtual page (with physical page from PMM) */
void* vmm_alloc_page(uint64_t flags);

/* Free virtual page */
void vmm_free_page(void* virt_addr);

/* Identity-map a device MMIO region, uncached (Phase 14). */
int vmm_map_mmio(uint64_t phys, uint64_t size);

/* The kernel's master PML4 (virtual pointer; its entries are shared by every
 * address space so the kernel half stays mapped under any CR3). */
uint64_t* vmm_kernel_pml4(void);

/* Assembly functions (from paging_asm.asm) */
extern void vmm_load_pml4(uint64_t pml4_phys);
extern void vmm_enable_paging(void);
extern uint64_t vmm_get_cr3(void);
extern void vmm_invlpg(uint64_t virt_addr);

#endif /* MAKHOS_VMM_H */
