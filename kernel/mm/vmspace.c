/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/vmspace.c
 * Per-process address spaces and copy-on-write. See mm/vmspace.h.
 *
 * Page tables are walked through the identity map: every table frame comes
 * from the PMM (low RAM, <= 1 GiB) which vmm_init maps 1:1, so a frame's
 * physical address doubles as a valid kernel pointer. The COW bit (PAGE_COW)
 * is a software PTE bit; on a write fault to a COW page the handler either
 * restores write (sole owner) or copies the frame.
 */

#include <mm/vmspace.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <lib/string.h>
#include <irq.h>

/* A table frame as a kernel pointer (identity map). */
static inline uint64_t* tbl(uint64_t phys) {
    return (uint64_t*)(uintptr_t)(phys & PTE_PHYS_MASK);
}

/* Ensure table[idx] points at a present (user) sub-table; create if asked. */
static uint64_t* ensure(uint64_t* table, int idx, int create) {
    if (!(table[idx] & PAGE_PRESENT)) {
        if (!create) return NULL;
        void* f = pmm_alloc_page();
        if (!f) return NULL;
        memset(f, 0, 4096);
        table[idx] = (uint64_t)(uintptr_t)f | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    } else {
        table[idx] |= PAGE_USER;
    }
    return tbl(table[idx]);
}

/* Pointer to the leaf PTE slot for `va`, creating the walk if asked. */
static uint64_t* leaf_slot(uint64_t pml4_phys, uint64_t va, int create) {
    uint64_t* pml4 = tbl(pml4_phys);
    uint64_t* pdpt = ensure(pml4, VMM_PML4_INDEX(va), create);
    if (!pdpt) return NULL;
    uint64_t* pd = ensure(pdpt, VMM_PDPT_INDEX(va), create);
    if (!pd) return NULL;
    uint64_t* pt = ensure(pd, VMM_PD_INDEX(va), create);
    if (!pt) return NULL;
    return &pt[VMM_PT_INDEX(va)];
}

int vmspace_create(address_space_t* as) {
    void* p = pmm_alloc_page();
    if (!p) return -1;
    uint64_t* np = tbl((uint64_t)(uintptr_t)p);
    memset(np, 0, 4096);

    /* Share every kernel PML4 entry by value; leave the user slot empty. */
    uint64_t* kp = vmm_kernel_pml4();
    for (int i = 0; i < 512; i++)
        if (i != USER_PML4_INDEX && (kp[i] & PAGE_PRESENT))
            np[i] = kp[i];

    as->pml4_phys = (uint64_t)(uintptr_t)p;
    as->refcount  = 1;            /* one thread until clone(CLONE_VM) shares it */
    return 0;
}

int vmspace_map(address_space_t* as, uint64_t va, uint64_t phys, uint64_t flags) {
    uint64_t* slot = leaf_slot(as->pml4_phys, va, 1);
    if (!slot) return -1;
    *slot = (phys & PTE_PHYS_MASK) | (flags & 0xFFF) | (flags & PAGE_NO_EXECUTE) |
            PAGE_PRESENT | PAGE_USER;
    return 0;
}

uint64_t vmspace_phys(address_space_t* as, uint64_t va) {
    uint64_t* slot = leaf_slot(as->pml4_phys, va, 0);
    if (!slot || !(*slot & PAGE_PRESENT)) return 0;
    return *slot & PTE_PHYS_MASK;
}

/* -------------------------------------------------------------------------- */
/* fork: deep-copy the user page-table structure, share data frames as COW     */
/* -------------------------------------------------------------------------- */

/* Copy one user table level. level 3 = PDPT, 2 = PD, 1 = PT (leaves). Returns
 * the new table's physical address, or 0 on OOM. */
static uint64_t copy_level(uint64_t src_phys, int level) {
    void* f = pmm_alloc_page();
    if (!f) return 0;
    uint64_t* dst = tbl((uint64_t)(uintptr_t)f);
    uint64_t* src = tbl(src_phys);
    memset(dst, 0, 4096);

    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PAGE_PRESENT)) continue;
        if (level == 1) {
            uint64_t e = src[i];
            uint64_t phys = e & PTE_PHYS_MASK;
            if (e & PAGE_WRITABLE) {           /* make writable pages COW in both */
                e = (e & ~(uint64_t)PAGE_WRITABLE) | PAGE_COW;
                src[i] = e;
            }
            dst[i] = e;
            page_incref(phys);
        } else {
            uint64_t sub = copy_level(src[i] & PTE_PHYS_MASK, level - 1);
            if (!sub) return 0;                 /* caller frees partial tree */
            dst[i] = (src[i] & 0xFFF) | sub;
        }
    }
    return (uint64_t)(uintptr_t)f;
}

int vmspace_fork(address_space_t* parent, address_space_t* child) {
    if (vmspace_create(child) != 0) return -1;

    uint64_t* pp = tbl(parent->pml4_phys);
    if (!(pp[USER_PML4_INDEX] & PAGE_PRESENT)) return 0;   /* nothing mapped yet */

    irqflags_t fl = local_irq_save();
    uint64_t sub = copy_level(pp[USER_PML4_INDEX] & PTE_PHYS_MASK, 3);
    if (!sub) { local_irq_restore(fl); vmspace_destroy(child); return -1; }

    uint64_t* cp = tbl(child->pml4_phys);
    cp[USER_PML4_INDEX] = (pp[USER_PML4_INDEX] & 0xFFF) | sub;
    local_irq_restore(fl);

    /* If the parent is the active space, its leaves just lost write permission,
     * so flush stale writable TLB entries. */
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_PHYS_MASK) == (parent->pml4_phys & PTE_PHYS_MASK))
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    return 0;
}

int vmspace_cow_fault(address_space_t* as, uint64_t va) {
    uint64_t* slot = leaf_slot(as->pml4_phys, va, 0);
    if (!slot || !(*slot & PAGE_PRESENT) || !(*slot & PAGE_COW)) return -1;

    uint64_t e = *slot;
    uint64_t old = e & PTE_PHYS_MASK;

    if (page_refcount(old) <= 1) {
        /* Sole owner: no copy needed, just become writable again. */
        *slot = (e & ~(uint64_t)PAGE_COW) | PAGE_WRITABLE;
    } else {
        void* nf = pmm_alloc_page();
        if (!nf) return -1;
        memcpy(tbl((uint64_t)(uintptr_t)nf), tbl(old), 4096);
        page_setref((uint64_t)(uintptr_t)nf, 1);
        *slot = ((uint64_t)(uintptr_t)nf & PTE_PHYS_MASK) |
                (e & 0xFFF & ~(uint64_t)PAGE_COW) | PAGE_WRITABLE |
                (e & PAGE_NO_EXECUTE);
        page_decref(old);
    }

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_PHYS_MASK) == (as->pml4_phys & PTE_PHYS_MASK))
        vmm_invlpg(va);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* unmap / protect (Phase 20-B: the primitives munmap and mprotect ride on)    */
/* -------------------------------------------------------------------------- */

/* Drop the mapping at `va`: clear the leaf PTE and release its frame (freeing
 * it when the last reference goes). Returns 0 if a page was unmapped, -1 if
 * nothing was mapped there. Flushes the TLB entry when `as` is active. */
int vmspace_unmap(address_space_t* as, uint64_t va) {
    uint64_t* slot = leaf_slot(as->pml4_phys, va, 0);
    if (!slot || !(*slot & PAGE_PRESENT)) return -1;
    uint64_t phys = *slot & PTE_PHYS_MASK;
    *slot = 0;
    page_decref(phys);

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_PHYS_MASK) == (as->pml4_phys & PTE_PHYS_MASK))
        vmm_invlpg(va);
    return 0;
}

/* Change the protection of the page at `va` to `flags` (the leaf bits
 * PAGE_WRITABLE / PAGE_NO_EXECUTE). The frame and PRESENT|USER are kept. A COW
 * page is never force-made-writable here: it stays COW so a write still takes
 * a private copy, which keeps fork isolation intact. Returns 0, or -1 if `va`
 * is not mapped. Flushes the TLB entry when `as` is active. */
int vmspace_protect(address_space_t* as, uint64_t va, uint64_t flags) {
    uint64_t* slot = leaf_slot(as->pml4_phys, va, 0);
    if (!slot || !(*slot & PAGE_PRESENT)) return -1;
    uint64_t e = *slot;
    uint64_t newe = (e & PTE_PHYS_MASK) | PAGE_PRESENT | PAGE_USER | (e & PAGE_COW);
    if ((flags & PAGE_WRITABLE) && !(e & PAGE_COW)) newe |= PAGE_WRITABLE;
    if (flags & PAGE_NO_EXECUTE) newe |= PAGE_NO_EXECUTE;
    *slot = newe;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PTE_PHYS_MASK) == (as->pml4_phys & PTE_PHYS_MASK))
        vmm_invlpg(va);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* destroy                                                                     */
/* -------------------------------------------------------------------------- */

static void free_level(uint64_t phys, int level) {
    uint64_t* t = tbl(phys);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PAGE_PRESENT)) continue;
        if (level == 1) page_decref(t[i] & PTE_PHYS_MASK);  /* a data frame */
        else free_level(t[i] & PTE_PHYS_MASK, level - 1);   /* a sub-table  */
    }
    pmm_free_page((void*)(uintptr_t)(phys & PTE_PHYS_MASK));
}

void vmspace_destroy(address_space_t* as) {
    if (!as || !as->pml4_phys) return;
    uint64_t* pml4 = tbl(as->pml4_phys);
    if (pml4[USER_PML4_INDEX] & PAGE_PRESENT)
        free_level(pml4[USER_PML4_INDEX] & PTE_PHYS_MASK, 3);
    pmm_free_page((void*)(uintptr_t)(as->pml4_phys & PTE_PHYS_MASK));
    as->pml4_phys = 0;
}

void vmspace_switch(address_space_t* as) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(as->pml4_phys & PTE_PHYS_MASK) : "memory");
}
