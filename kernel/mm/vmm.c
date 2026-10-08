/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - vmm.c
 * Virtual Memory Manager implementation
 * Implements 4-level paging (PML4 -> PDPT -> PD -> PT) for x86_64
 */

#include "mm/vmm.h"
#include "mm/pmm.h"
#include <arch/cpu.h>
#include "vga.h"
#include "kernel.h"
#include "klog.h"

/* Static kernel PML4 table - must be 4KB aligned */
static uint64_t kernel_pml4[PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));

/* Higher-half direct map (HHDM) page tables. One PDPT entry + one PD of 2 MiB
 * huge pages covers up to 1 GiB of RAM at HHDM_BASE. Static so they live in the
 * kernel image (always reachable) and have fixed addresses the later high relink
 * can translate with V2P. */
static uint64_t hhdm_pdpt[PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t hhdm_pd[PAGE_TABLE_ENTRIES]   __attribute__((aligned(4096)));

/* Current PML4 physical address */
static uint64_t current_pml4_phys = 0;

/* Next available virtual address for vmm_alloc_page (higher half) */
static uint64_t next_virt_addr = VMM_ALLOC_BASE;

/* Helper function prototypes */
static uint64_t* get_or_create_pdpt(uint64_t virt_addr);
static uint64_t* get_or_create_pd(uint64_t virt_addr);
static uint64_t* get_or_create_pt(uint64_t virt_addr);
static void memset64(uint64_t* dest, uint64_t val, size_t count);

/**
 * memset64 - Fill 64-bit aligned memory
 */
static void memset64(uint64_t* dest, uint64_t val, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dest[i] = val;
    }
}

/**
 * get_or_create_pdpt - Get or create PDPT for virtual address
 * Returns: Pointer to PDPT table (virtual address), or NULL on failure
 */
static uint64_t* get_or_create_pdpt(uint64_t virt_addr) {
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    
    /* Check if PML4 entry exists */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        /* Allocate new PDPT */
        void* new_pdpt = pmm_alloc_page();
        if (new_pdpt == NULL) return NULL;
        
        /* Clear the new table (through the HHDM) */
        memset64((uint64_t*)P2V((uint64_t)(uintptr_t)new_pdpt), 0, PAGE_TABLE_ENTRIES);

        /* Set PML4 entry: physical address + flags */
        kernel_pml4[pml4_idx] = (uint64_t)(uintptr_t)new_pdpt | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Get physical address of PDPT, reach it through the HHDM */
    uint64_t pdpt_phys = kernel_pml4[pml4_idx] & ~0xFFF;
    return (uint64_t*)P2V(pdpt_phys);
}

/**
 * get_or_create_pd - Get or create Page Directory for virtual address
 * Returns: Pointer to PD table (virtual address), or NULL on failure
 */
static uint64_t* get_or_create_pd(uint64_t virt_addr) {
    /* Get PDPT first */
    uint64_t* pdpt = get_or_create_pdpt(virt_addr);
    if (pdpt == NULL) return NULL;
    
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    /* Check if PDPT entry exists */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        /* Allocate new PD */
        void* new_pd = pmm_alloc_page();
        if (new_pd == NULL) return NULL;
        
        /* Clear the new table (through the HHDM) */
        memset64((uint64_t*)P2V((uint64_t)(uintptr_t)new_pd), 0, PAGE_TABLE_ENTRIES);

        /* Set PDPT entry: physical address + flags */
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)new_pd | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Get physical address of PD, reach it through the HHDM */
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    return (uint64_t*)P2V(pd_phys);
}

/**
 * get_or_create_pt - Get or create Page Table for virtual address
 * Returns: Pointer to PT table (virtual address), or NULL on failure
 */
static uint64_t* get_or_create_pt(uint64_t virt_addr) {
    /* Get PD first */
    uint64_t* pd = get_or_create_pd(virt_addr);
    if (pd == NULL) return NULL;
    
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    /* Check if PD entry exists */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        /* Allocate new PT */
        void* new_pt = pmm_alloc_page();
        if (new_pt == NULL) return NULL;
        
        /* Clear the new table (through the HHDM) */
        memset64((uint64_t*)P2V((uint64_t)(uintptr_t)new_pt), 0, PAGE_TABLE_ENTRIES);

        /* Set PD entry: physical address + flags */
        pd[pd_idx] = (uint64_t)(uintptr_t)new_pt | PAGE_PRESENT | PAGE_WRITABLE;
    }

    /* Get physical address of PT, reach it through the HHDM */
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    return (uint64_t*)P2V(pt_phys);
}

/**
 * Simple hex print helper for debug output
 */
static void vmm_print_hex(uint64_t value) {
    const char* hex = "0123456789ABCDEF";
    char buf[20];
    int i = 0;
    
    if (value == 0) {
        terminal_writestring("0x0");
        return;
    }
    
    buf[0] = '0';
    buf[1] = 'x';
    i = 2;
    
    /* Find highest nibble */
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int nibble = (value >> shift) & 0xF;
        if (nibble != 0 || started || shift == 0) {
            buf[i++] = hex[nibble];
            started = 1;
        }
    }
    buf[i] = '\0';
    terminal_writestring(buf);
}

/**
 * vmm_init - Initialize the Virtual Memory Manager
 * Creates kernel PML4, copies boot mappings, sets recursive mapping, switches CR3
 * 
 * CRITICAL: boot.asm already enables paging before kernel_main is called.
 * We must NOT disable/re-enable paging. We only switch CR3 to our new PML4.
 * Before switching, we must ensure our new PML4 has all necessary mappings
 * to continue execution (especially for the kernel code/data itself).
 */
void vmm_init(void) {
    terminal_writestring("[VMM] Starting initialization...\n");
    
    /* Get the current boot PML4 from CR3 */
    uint64_t boot_pml4_phys = vmm_get_cr3();
    terminal_writestring("[VMM] Boot PML4 at: ");
    vmm_print_hex(boot_pml4_phys);
    terminal_writestring("\n");
    
    /* Clear kernel PML4 */
    memset64(kernel_pml4, 0, PAGE_TABLE_ENTRIES);
    terminal_writestring("[VMM] Kernel PML4 cleared\n");
    
    /* 
     * Copy boot page table mappings to our new kernel PML4.
     * This is CRITICAL - the boot page tables have identity mapping
     * for 0-4MB. We must preserve these mappings or the CPU will crash
     * when we switch CR3.
     */
    terminal_writestring("[VMM] Copying boot page table mappings...\n");
    uint64_t* boot_pml4 = (uint64_t*)(uintptr_t)boot_pml4_phys;
    
    /* Copy all present entries from boot PML4 to kernel PML4 */
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (boot_pml4[i] & PAGE_PRESENT) {
            kernel_pml4[i] = boot_pml4[i];
        }
    }
    terminal_writestring("[VMM] Boot mappings copied\n");
    
    /*
     * CRITICAL FIX: Extend identity mapping to cover 0-16MB.
     * 
     * The boot page tables only map 0-4MB, but PMM allocates pages
     * starting at 0x400000. When we try to access those pages to
     * clear them, we get a page fault because they're not mapped.
     *
     * We need to set up page tables for 4MB-16MB before switching CR3.
     */
    /*
     * Identity-map all of physical RAM with 2MB huge pages.
     *
     * The boot page tables only map the first 4MB, but the PMM hands out
     * frames from anywhere in RAM. Any kernel access to such a frame (e.g.
     * zeroing a freshly allocated page table, or the direct pointers the heap
     * uses) must be backed by a mapping. The boot PD at pdpt[0] holds 512
     * entries = 1GB of coverage, so we fill it up to the RAM size, capped at
     * 1GB (enough for our QEMU configs; a higher-half direct map comes later).
     */
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(kernel_pml4[0] & ~0xFFF);
    uint64_t* pd   = (uint64_t*)(uintptr_t)(pdpt[0] & ~0xFFF);

    uint64_t ram = pmm_get_total_memory();
    uint64_t map_limit = 1024ULL * 1024 * 1024;   /* cap at 1GB (512 * 2MB) */
    if (ram > map_limit) ram = map_limit;

    uint64_t entries = (ram + 0x1FFFFF) / 0x200000;  /* round up to 2MB */
    if (entries > PAGE_TABLE_ENTRIES) entries = PAGE_TABLE_ENTRIES;

    for (uint64_t i = 2; i < entries; i++) {
        if (!(pd[i] & PAGE_PRESENT)) {
            pd[i] = (i * 0x200000) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }
    }
    KLOG_I("VMM", "identity-mapped %lu MB of RAM\n", (entries * 2));

    /*
     * Build the higher-half direct map (HHDM) at HHDM_BASE: the same RAM, mapped
     * 1:1 with 2 MiB huge pages, but in PML4 slot 256. It aliases the low
     * identity map today; once every physical-frame access goes through P2V()
     * the low identity map can be dropped from user address spaces, freeing the
     * entire low canonical half. The HHDM tables are kernel statics, so their
     * virtual address equals their physical address while the kernel is still
     * identity-mapped low (that changes with V2P at the high-relink brick).
     */
    memset64(hhdm_pdpt, 0, PAGE_TABLE_ENTRIES);
    memset64(hhdm_pd, 0, PAGE_TABLE_ENTRIES);
    for (uint64_t i = 0; i < entries; i++) {
        hhdm_pd[i] = (i * 0x200000) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
    }
    /* The HHDM tables are kernel statics linked in the higher half, so the
     * physical address a page-table entry must store is KV2P() of the symbol. */
    hhdm_pdpt[0] = KV2P(hhdm_pd) | PAGE_PRESENT | PAGE_WRITABLE;
    kernel_pml4[VMM_PML4_INDEX(HHDM_BASE)] = KV2P(hhdm_pdpt) | PAGE_PRESENT | PAGE_WRITABLE;
    KLOG_I("VMM", "HHDM at %p maps %lu MB\n", (void*)HHDM_BASE, (entries * 2));

    /*
     * PML4 slot 511 now holds the higher-half kernel window (set by boot.asm and
     * copied in above from the boot PML4), so there is no recursive self-map —
     * nothing ever used it. CR3 wants the PML4's physical address, and the table
     * is a kernel static, so KV2P() of its symbol.
     */
    uint64_t pml4_phys = KV2P(kernel_pml4);
    
    /* Store current PML4 physical address */
    current_pml4_phys = pml4_phys;
    
    /* 
     * Load our new PML4 into CR3.
     * This is safe now because we've copied all the boot mappings,
     * so the identity mapping for kernel code/data is preserved.
     * 
     * NOTE: We do NOT call vmm_enable_paging() because:
     * 1. boot.asm already enabled paging before kernel_main
     * 2. Re-enabling paging while it's already on can cause issues
     * 3. We only need to switch CR3, not reconfigure CR0/CR4/EFER
     */
    terminal_writestring("[VMM] Switching to kernel PML4 at: ");
    vmm_print_hex(pml4_phys);
    terminal_writestring("\n");
    vmm_load_pml4(pml4_phys);
    terminal_writestring("[VMM] CR3 loaded successfully\n");
    
    /* Phase 17: enable No-Execute (EFER.NXE, bit 11) so leaf PTEs may set the
     * NX bit and W^X can be enforced. CPUID leaf 0x80000001 EDX bit 20 reports
     * NX support; QEMU provides it. */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax),"=b"(ebx),"=c"(ecx),"=d"(edx) : "a"(0x80000001u));
    if (edx & (1u << 20)) {
        wrmsr(IA32_EFER, rdmsr(IA32_EFER) | (1u << 11));
        terminal_writestring("[VMM] NX enabled (EFER.NXE)\n");
    } else {
        terminal_writestring("[VMM] NX not supported by CPU\n");
    }
    terminal_writestring("[VMM] Initialized with 4-level paging\n");
}

/**
 * vmm_create_address_space - Create a new address space (new PML4)
 * Returns: Physical address of new PML4, or 0 on failure
 */
uint64_t* vmm_kernel_pml4(void) { return kernel_pml4; }

uint64_t vmm_create_address_space(void) {
    /* Allocate a new PML4 */
    void* new_pml4 = pmm_alloc_page();
    if (new_pml4 == NULL) return 0;
    uint64_t* np = (uint64_t*)P2V((uint64_t)(uintptr_t)new_pml4);   /* edit via HHDM */

    /* Clear it */
    memset64(np, 0, PAGE_TABLE_ENTRIES);

    /* Copy kernel mappings (higher half) from current PML4 */
    /* Entry 256-511 are for kernel space */
    for (int i = 256; i < PAGE_TABLE_ENTRIES; i++) {
        np[i] = kernel_pml4[i];
    }

    return (uint64_t)(uintptr_t)new_pml4;
}

/**
 * vmm_switch_address_space - Switch to a different address space
 * @pml4_phys: Physical address of PML4 to switch to
 */
void vmm_switch_address_space(uint64_t pml4_phys) {
    current_pml4_phys = pml4_phys;
    vmm_load_pml4(pml4_phys);
}

/**
 * vmm_map_page - Map a physical page to a virtual address
 * @virt_addr: Virtual address to map to
 * @phys_addr: Physical address to map
 * @flags: Page flags (PAGE_PRESENT, PAGE_WRITABLE, etc.)
 * Returns: 0 on success, -1 on failure
 */
/**
 * vmm_map_user_page - map a ring-3-accessible page.
 *
 * A page is only reachable from ring 3 if the USER bit is set on EVERY level
 * of the walk, not just the leaf. The generic get_or_create_* helpers create
 * intermediate tables with PRESENT|WRITABLE only, so we OR in PAGE_USER at each
 * level here. The leaf keeps the low flag bits AND the NX bit (63), so W^X can
 * be enforced now that EFER.NXE is on (vmm_init). Intermediate tables stay
 * executable (NX only matters on the leaf for 4 KiB pages).
 */
int vmm_map_user_page(uint64_t va, uint64_t phys, uint64_t flags) {
    if ((va & 0xFFF) || (phys & 0xFFF)) return -1;

    uint64_t* pdpt = get_or_create_pdpt(va);
    if (!pdpt) return -1;
    kernel_pml4[VMM_PML4_INDEX(va)] |= PAGE_USER;

    uint64_t* pd = get_or_create_pd(va);
    if (!pd) return -1;
    pdpt[VMM_PDPT_INDEX(va)] |= PAGE_USER;

    uint64_t* pt = get_or_create_pt(va);
    if (!pt) return -1;
    pd[VMM_PD_INDEX(va)] |= PAGE_USER;

    pt[VMM_PT_INDEX(va)] = (phys & ~0xFFF) | (flags & 0xFFF) |
                           (flags & PAGE_NO_EXECUTE) | PAGE_PRESENT | PAGE_USER;
    vmm_invlpg(va);
    return 0;
}

int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    /* Ensure addresses are page-aligned */
    if (virt_addr & 0xFFF) return -1;
    if (phys_addr & 0xFFF) return -1;
    
    /* Get or create the page table */
    uint64_t* pt = get_or_create_pt(virt_addr);
    if (pt == NULL) return -1;
    
    /* Set the page table entry */
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    pt[pt_idx] = (phys_addr & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
    
    /* Invalidate TLB for this page */
    vmm_invlpg(virt_addr);
    
    return 0;
}

/**
 * vmm_unmap_page - Unmap a virtual page
 * @virt_addr: Virtual address to unmap
 * Returns: 0 on success, -1 on failure
 */
int vmm_unmap_page(uint64_t virt_addr) {
    /* Ensure address is page-aligned */
    if (virt_addr & 0xFFF) return -1;
    
    /* Get PML4 index */
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    
    /* Check if PML4 entry exists */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        return -1;  /* Not mapped */
    }
    
    /* Get PDPT */
    uint64_t pdpt_phys = kernel_pml4[pml4_idx] & ~0xFFF;
    uint64_t* pdpt = (uint64_t*)P2V(pdpt_phys);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    /* Check if PDPT entry exists */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return -1;  /* Not mapped */
    }
    
    /* Get PD */
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    uint64_t* pd = (uint64_t*)P2V(pd_phys);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    /* Check if PD entry exists */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return -1;  /* Not mapped */
    }
    
    /* Get PT */
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    uint64_t* pt = (uint64_t*)P2V(pt_phys);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    
    /* Clear the page table entry */
    pt[pt_idx] = 0;
    
    /* Invalidate TLB for this page */
    vmm_invlpg(virt_addr);
    
    return 0;
}

/**
 * vmm_get_physical - Get the physical address for a virtual address
 * @virt_addr: Virtual address to translate
 * Returns: Physical address, or 0 if not mapped
 */
uint64_t vmm_get_physical(uint64_t virt_addr) {
    uint64_t page_offset = VMM_PAGE_OFFSET(virt_addr);
    
    /* Get PML4 index */
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    
    /* Check if PML4 entry exists */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        return 0;  /* Not mapped */
    }
    
    /* Get PDPT */
    uint64_t pdpt_phys = kernel_pml4[pml4_idx] & ~0xFFF;
    uint64_t* pdpt = (uint64_t*)P2V(pdpt_phys);
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    /* Check if PDPT entry exists */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return 0;  /* Not mapped */
    }
    
    /* Check for huge page (1GB) at PDPT level */
    if (pdpt[pdpt_idx] & PAGE_HUGE) {
        /* 1GB huge page - bits 30-51 contain physical base */
        uint64_t phys_base = pdpt[pdpt_idx] & ~0x3FFFFFFF;
        return phys_base | (virt_addr & 0x3FFFFFFF);
    }
    
    /* Get PD */
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    uint64_t* pd = (uint64_t*)P2V(pd_phys);
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    /* Check if PD entry exists */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return 0;  /* Not mapped */
    }
    
    /* Check for huge page (2MB) at PD level */
    if (pd[pd_idx] & PAGE_HUGE) {
        /* 2MB huge page - bits 21-51 contain physical base */
        uint64_t phys_base = pd[pd_idx] & ~0x1FFFFF;
        return phys_base | (virt_addr & 0x1FFFFF);
    }
    
    /* Get PT */
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    uint64_t* pt = (uint64_t*)P2V(pt_phys);
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    
    /* Check if page table entry exists */
    if (!(pt[pt_idx] & PAGE_PRESENT)) {
        return 0;  /* Not mapped */
    }
    
    /* Get physical page base address and add offset */
    uint64_t phys_page = pt[pt_idx] & ~0xFFF;
    return phys_page | page_offset;
}

/**
 * vmm_alloc_page - Allocate a virtual page with a physical page
 * @flags: Page flags (PAGE_PRESENT, PAGE_WRITABLE, etc.)
 * Returns: Virtual address of allocated page, or NULL on failure
 */
void* vmm_alloc_page(uint64_t flags) {
    /* Allocate a physical page */
    void* phys_page = pmm_alloc_page();
    if (phys_page == NULL) return NULL;
    
    /* Get next available virtual address */
    uint64_t virt_addr = next_virt_addr;
    next_virt_addr += PAGE_SIZE;
    
    /* Map the page */
    if (vmm_map_page(virt_addr, (uint64_t)(uintptr_t)phys_page, flags) != 0) {
        /* Failed to map - free physical page */
        pmm_free_page(phys_page);
        next_virt_addr -= PAGE_SIZE;  /* Rollback */
        return NULL;
    }
    
    return (void*)(uintptr_t)virt_addr;
}

/**
 * vmm_free_page - Free a virtual page and its associated physical page
 * @virt_addr: Virtual address of page to free
 */
void vmm_free_page(void* virt_addr) {
    if (virt_addr == NULL) return;
    
    uint64_t virt = (uint64_t)(uintptr_t)virt_addr;
    
    /* Get physical address */
    uint64_t phys = vmm_get_physical(virt);
    if (phys == 0) return;  /* Not mapped */
    
    /* Unmap the virtual page */
    vmm_unmap_page(virt);
    
    /* Free the physical page */
    pmm_free_page((void*)(uintptr_t)phys);
}

/**
 * vmm_map_mmio - Identity-map a device MMIO region, uncached.
 * @phys: physical base (need not be page aligned)
 * @size: length in bytes
 * Returns: 0 on success, -1 on failure.
 *
 * Phase 14: PCI BARs usually live above RAM (e.g. ~0xFEB80000 in QEMU), outside
 * the RAM identity map, and device registers must never be cached, so each page
 * is mapped virt == phys with PCD|PWT. Pages already mapped (e.g. inside the RAM
 * identity map) are left alone.
 */
int vmm_map_mmio(uint64_t phys, uint64_t size) {
    uint64_t start = phys & ~0xFFFULL;
    uint64_t end = (phys + size + 0xFFF) & ~0xFFFULL;
    for (uint64_t p = start; p < end; p += 0x1000) {
        if (vmm_get_physical(p) == p) continue;   /* already identity-mapped */
        if (vmm_map_page(p, p, PAGE_PRESENT | PAGE_WRITABLE |
                               PAGE_CACHE_DISABLE | PAGE_WRITETHROUGH) != 0) {
            return -1;
        }
    }
    return 0;
}
