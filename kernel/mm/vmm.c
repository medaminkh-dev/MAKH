/**
 * MakhOS - vmm.c
 * Virtual Memory Manager implementation
 * Implements 4-level paging (PML4 -> PDPT -> PD -> PT) for x86_64
 */

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "vga.h"
#include "kernel.h"

/* Static kernel PML4 table - must be 4KB aligned */
static uint64_t kernel_pml4[PAGE_TABLE_ENTRIES] __attribute__((aligned(4096)));

/* Current PML4 physical address */
static uint64_t current_pml4_phys = 0;

/* Next available virtual address for vmm_alloc_page (higher half) */
static uint64_t next_virt_addr = KERNEL_HIGHER_HALF_BASE;

/* Helper function prototypes */
static uint64_t* get_or_create_pdpt(uint64_t virt_addr, uint64_t flags);
static uint64_t* get_or_create_pd(uint64_t virt_addr, uint64_t flags);
static uint64_t* get_or_create_pt(uint64_t virt_addr, uint64_t flags);
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
static uint64_t* get_or_create_pdpt(uint64_t virt_addr, uint64_t flags) {
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    
    /* Check if PML4 entry exists */
    if (!(kernel_pml4[pml4_idx] & PAGE_PRESENT)) {
        /* Allocate new PDPT */
        void* new_pdpt = pmm_alloc_page();
        if (new_pdpt == NULL) return NULL;
        
        /* Clear the new table */
        memset64((uint64_t*)new_pdpt, 0, PAGE_TABLE_ENTRIES);
        
        /* Set PML4 entry: physical address + flags */
        uint64_t entry_flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (flags & PAGE_USER) {
            entry_flags |= PAGE_USER;
        }
        kernel_pml4[pml4_idx] = (uint64_t)(uintptr_t)new_pdpt | entry_flags;
    } else if (flags & PAGE_USER) {
        /* Entry exists - ensure USER flag is set if requested */
        kernel_pml4[pml4_idx] |= PAGE_USER;
    }
    
    /* Get physical address of PDPT, convert to virtual (identity mapped) */
    uint64_t pdpt_phys = kernel_pml4[pml4_idx] & ~0xFFF;
    return (uint64_t*)(uintptr_t)pdpt_phys;
}

/**
 * get_or_create_pd - Get or create Page Directory for virtual address
 * Returns: Pointer to PD table (virtual address), or NULL on failure
 */
static uint64_t* get_or_create_pd(uint64_t virt_addr, uint64_t flags) {
    /* Get PDPT first */
    uint64_t* pdpt = get_or_create_pdpt(virt_addr, flags);
    if (pdpt == NULL) return NULL;
    
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    /* Check if PDPT entry exists */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        /* Allocate new PD */
        void* new_pd = pmm_alloc_page();
        if (new_pd == NULL) return NULL;
        
        /* Clear the new table */
        memset64((uint64_t*)new_pd, 0, PAGE_TABLE_ENTRIES);
        
        /* Set PDPT entry: physical address + flags */
        uint64_t entry_flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (flags & PAGE_USER) {
            entry_flags |= PAGE_USER;
        }
        pdpt[pdpt_idx] = (uint64_t)(uintptr_t)new_pd | entry_flags;
    } else if (flags & PAGE_USER) {
        /* Entry exists - ensure USER flag is set if requested */
        pdpt[pdpt_idx] |= PAGE_USER;
    }
    
    /* Get physical address of PD, convert to virtual (identity mapped) */
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    return (uint64_t*)(uintptr_t)pd_phys;
}

/**
 * get_or_create_pt - Get or create Page Table for virtual address
 * Returns: Pointer to PT table (virtual address), or NULL on failure
 */
static uint64_t* get_or_create_pt(uint64_t virt_addr, uint64_t flags) {
    /* Get PD first */
    uint64_t* pd = get_or_create_pd(virt_addr, flags);
    if (pd == NULL) return NULL;
    
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    /* Check if PD entry exists */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        /* Allocate new PT */
        void* new_pt = pmm_alloc_page();
        if (new_pt == NULL) return NULL;
        
        /* Clear the new table */
        memset64((uint64_t*)new_pt, 0, PAGE_TABLE_ENTRIES);
        
        /* Set PD entry: physical address + flags */
        uint64_t entry_flags = PAGE_PRESENT | PAGE_WRITABLE;
        if (flags & PAGE_USER) entry_flags |= PAGE_USER;
        pd[pd_idx] = (uint64_t)(uintptr_t)new_pt | entry_flags;
    } else if (flags & PAGE_USER) {
        /* Entry exists - ensure USER flag is set if requested */
        pd[pd_idx] |= PAGE_USER;
    }
    
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    return (uint64_t*)(uintptr_t)pt_phys;
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
    terminal_writestring("[VMM] Extending identity mapping to 16MB...\n");
    
    /* Create PDPT for identity mapping (PML4[0]) */
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(kernel_pml4[0] & ~0xFFF);
    
    /* Check if we need to extend the PD */
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[0] & ~0xFFF);
    
    /* Map additional 2MB pages: 0x400000, 0x600000, 0x800000, 0xA00000, 0xC00000, 0xE00000 */
    for (int i = 2; i < 8; i++) {
        if (!(pd[i] & PAGE_PRESENT)) {
            /* Use 2MB huge pages for simplicity */
            pd[i] = ((uint64_t)i * 0x200000) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }
    }
    terminal_writestring("[VMM] Identity mapping extended to 16MB\n");
    
    /* 
     * Set up recursive mapping: PML4[511] = &PML4 | flags
     * This allows us to access page tables through the higher half
     * at virtual address 0xFFFFFFFFFFFFF000 + indices
     */
    uint64_t pml4_phys = (uint64_t)(uintptr_t)kernel_pml4;
    kernel_pml4[RECURSIVE_PML4_INDEX] = pml4_phys | PAGE_PRESENT | PAGE_WRITABLE;
    terminal_writestring("[VMM] Recursive mapping set (PML4[511])\n");
    
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
    
    terminal_writestring("[VMM] Initialized with 4-level paging\n");
}

/**
 * vmm_create_address_space - Create a new address space (new PML4)
 * Returns: Physical address of new PML4, or 0 on failure
 */
uint64_t vmm_create_address_space(void) {
    /* Allocate a new PML4 */
    void* new_pml4 = pmm_alloc_page();
    if (new_pml4 == NULL) return 0;
    
    /* Clear it */
    memset64((uint64_t*)new_pml4, 0, PAGE_TABLE_ENTRIES);
    
    /* Copy kernel mappings (higher half) from current PML4 */
    /* Entry 256-511 are for kernel space */
    for (int i = 256; i < PAGE_TABLE_ENTRIES; i++) {
        ((uint64_t*)new_pml4)[i] = kernel_pml4[i];
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
int vmm_map_page(uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    /* Ensure addresses are page-aligned */
    if (virt_addr & 0xFFF) return -1;
    if (phys_addr & 0xFFF) return -1;
    
    /* Get or create the page table */
    uint64_t* pt = get_or_create_pt(virt_addr, flags);
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
    uint64_t* pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    /* Check if PDPT entry exists */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return -1;  /* Not mapped */
    }
    
    /* Get PD */
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    uint64_t* pd = (uint64_t*)(uintptr_t)pd_phys;
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    /* Check if PD entry exists */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return -1;  /* Not mapped */
    }
    
    /* Get PT */
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    uint64_t* pt = (uint64_t*)(uintptr_t)pt_phys;
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
    uint64_t* pdpt = (uint64_t*)(uintptr_t)pdpt_phys;
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
    uint64_t* pd = (uint64_t*)(uintptr_t)pd_phys;
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
    uint64_t* pt = (uint64_t*)(uintptr_t)pt_phys;
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
 * vmm_dump_page_tables - Debug function to dump page table entries for a virtual address
 * @virt_addr: Virtual address to dump page table info for
 * 
 * This function helps verify that page table entries have the USER flag set correctly.
 */
void vmm_dump_page_tables(uint64_t virt_addr) {
    char buf[32];
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    
    terminal_writestring("\n[VMM DEBUG] Dumping for: ");
    uint64_to_hex(virt_addr, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    terminal_writestring("[VMM DEBUG] CR3=");
    uint64_to_hex(cr3, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // PML4
    uint64_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint64_t* pml4 = (uint64_t*)cr3;
    terminal_writestring("[VMM DEBUG] PML4[");
    uint64_to_hex(pml4_idx, buf);
    terminal_writestring(buf);
    terminal_writestring("]=");
    uint64_to_hex(pml4[pml4_idx], buf);
    terminal_writestring("\n");
    
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        terminal_writestring("[VMM DEBUG] NOT PRESENT\n");
        return;
    }
    
    // Check USER flag
    if (pml4[pml4_idx] & PAGE_USER) {
        terminal_writestring("[VMM DEBUG] USER flag set\n");
    } else {
        terminal_writestring("[VMM DEBUG] USER flag MISSING\n");
    }
    
    // PDPT
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFF;
    uint64_t* pdpt = (uint64_t*)pdpt_phys;
    uint64_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    
    terminal_writestring("[VMM DEBUG] PDPT[");
    uint64_to_hex(pdpt_idx, buf);
    terminal_writestring(buf);
    terminal_writestring("]=");
    uint64_to_hex(pdpt[pdpt_idx], buf);
    terminal_writestring("\n");
    
    if (pdpt[pdpt_idx] & PAGE_USER) {
        terminal_writestring("[VMM DEBUG] USER flag set\n");
    } else {
        terminal_writestring("[VMM DEBUG] USER flag MISSING\n");
    }
    
    // PD
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    uint64_t* pd = (uint64_t*)pd_phys;
    uint64_t pd_idx = VMM_PD_INDEX(virt_addr);
    
    terminal_writestring("[VMM DEBUG] PD[");
    uint64_to_hex(pd_idx, buf);
    terminal_writestring(buf);
    terminal_writestring("]=");
    uint64_to_hex(pd[pd_idx], buf);
    terminal_writestring("\n");
    
    if (pd[pd_idx] & PAGE_USER) {
        terminal_writestring("[VMM DEBUG] USER flag set\n");
    } else {
        terminal_writestring("[VMM DEBUG] USER flag MISSING\n");
    }
    
    // PT
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    uint64_t* pt = (uint64_t*)pt_phys;
    uint64_t pt_idx = VMM_PT_INDEX(virt_addr);
    
    terminal_writestring("[VMM DEBUG] PT[");
    uint64_to_hex(pt_idx, buf);
    terminal_writestring(buf);
    terminal_writestring("]=");
    uint64_to_hex(pt[pt_idx], buf);
    terminal_writestring("\n");
    
    if (pt[pt_idx] & PAGE_USER) {
        terminal_writestring("[VMM DEBUG] USER flag set\n");
    } else {
        terminal_writestring("[VMM DEBUG] USER flag MISSING\n");
    }
    
    uint64_t phys_page = pt[pt_idx] & ~0xFFF;
    terminal_writestring("[VMM DEBUG] Phys page=");
    uint64_to_hex(phys_page, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}
