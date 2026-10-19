/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_vm.c
 * Phase 17: address spaces, copy-on-write and per-frame refcounts.
 *
 * These exercise the COW machinery directly (no CR3 switch): map a frame into
 * a space, fork it, fault a copy out, and check isolation and frame accounting.
 */

#include <ktest.h>
#include <mm/vmspace.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <elf.h>
#include <errno.h>
#include <irq.h>

#define TEST_VA  0x0000200000010000ULL   /* inside the user PML4 slot (64) */

/* Physical frame as a kernel pointer (identity map). */
static volatile uint8_t* kptr(uint64_t phys) {
    return (volatile uint8_t*)(uintptr_t)(phys & PTE_PHYS_MASK);
}

KTEST(vm, page_refcount_frees_on_last_put) {
    void* f = pmm_alloc_page();
    KASSERT_TEST(f != NULL);
    uint64_t phys = (uint64_t)(uintptr_t)f;
    page_setref(phys, 1);
    page_incref(phys);                         /* 2 */
    KEXPECT_EQ(page_refcount(phys), 2u);
    KEXPECT_EQ(page_decref(phys), 1u);         /* still held */
    KEXPECT_EQ(page_refcount(phys), 1u);
    KEXPECT_EQ(page_decref(phys), 0u);         /* frees the frame */
}

KTEST(vm, cow_fork_isolates_child_writes) {
    size_t free_before = pmm_get_free_memory();

    address_space_t parent, child;
    KASSERT_TEST(vmspace_create(&parent) == 0);

    void* f = pmm_alloc_page();
    KASSERT_TEST(f != NULL);
    uint64_t pf = (uint64_t)(uintptr_t)f;
    page_setref(pf, 1);
    memset((void*)kptr(pf), 0xAA, 4096);
    KASSERT_TEST(vmspace_map(&parent, TEST_VA, pf, PAGE_WRITABLE) == 0);

    /* Fork: both see the same frame, read-only + COW, refcount 2. */
    KASSERT_TEST(vmspace_fork(&parent, &child) == 0);
    KEXPECT_EQ(vmspace_phys(&parent, TEST_VA), pf);
    KEXPECT_EQ(vmspace_phys(&child, TEST_VA), pf);
    KEXPECT_EQ(page_refcount(pf), 2u);

    /* The child writes -> COW fault -> private copy. */
    KEXPECT_EQ(vmspace_cow_fault(&child, TEST_VA), 0);
    uint64_t cf = vmspace_phys(&child, TEST_VA);
    KEXPECT_NE(cf, pf);                         /* child moved to a new frame */
    KEXPECT_EQ(vmspace_phys(&parent, TEST_VA), pf);  /* parent did not */
    KEXPECT_EQ(page_refcount(pf), 1u);          /* parent is sole owner again */

    /* Write a different pattern into the child's copy; parent's frame is intact. */
    memset((void*)kptr(cf), 0xBB, 4096);
    KEXPECT_EQ(kptr(pf)[0], 0xAA);
    KEXPECT_EQ(kptr(cf)[0], 0xBB);

    vmspace_destroy(&child);
    vmspace_destroy(&parent);                   /* frees pf (last ref) */

    KEXPECT_EQ(pmm_get_free_memory(), free_before);  /* nothing leaked */
}

KTEST(vm, fork_many_times_without_leak) {
    size_t free_before = pmm_get_free_memory();

    address_space_t parent;
    KASSERT_TEST(vmspace_create(&parent) == 0);
    void* f = pmm_alloc_page();
    KASSERT_TEST(f != NULL);
    uint64_t pf = (uint64_t)(uintptr_t)f;
    page_setref(pf, 1);
    KASSERT_TEST(vmspace_map(&parent, TEST_VA, pf, PAGE_WRITABLE) == 0);

    for (int i = 0; i < 1000; i++) {
        address_space_t child;
        KASSERT_TEST(vmspace_fork(&parent, &child) == 0);
        KEXPECT_EQ(page_refcount(pf), 2u);      /* parent + this child */
        vmspace_destroy(&child);
        KEXPECT_EQ(page_refcount(pf), 1u);      /* back to parent only */
    }

    vmspace_destroy(&parent);
    KEXPECT_EQ(pmm_get_free_memory(), free_before);  /* 1000 forks, no leak */
}

/* The device-MMIO window (vmm_map_mmio): a BAR must map into the higher half so
 * the mapping is shared into every address space — the kernel services a device
 * IRQ even while a user CR3 is live. Uses a real RAM frame as a stand-in BAR
 * (the test QEMU attaches no NIC), which is enough to exercise the window. */
KTEST(mmio, window_is_high_and_shared) {
    void* f = pmm_alloc_page();
    KASSERT_TEST(f != NULL);
    uint64_t phys = (uint64_t)(uintptr_t)f & PTE_PHYS_MASK;

    uint64_t va = vmm_map_mmio(phys, 4096);
    KASSERT_TEST(va != 0);
    /* Lands in the higher-half MMIO window (PML4 slot 258), not the user half. */
    KEXPECT_EQ(va >= MMIO_WINDOW_BASE && va < MMIO_WINDOW_END, 1);
    /* Resolves to exactly the frame requested, keeping the sub-page offset. */
    KEXPECT_EQ(vmm_get_physical(va & ~0xFFFULL), phys);

    /* A write through the window reaches that frame (checked via the HHDM alias). */
    *(volatile uint32_t*)(uintptr_t)va = 0xC0FFEE42u;
    KEXPECT_EQ(*(volatile uint32_t*)P2V(phys), 0xC0FFEE42u);

    /* The key property: a freshly created address space shares the window's
     * top-level PML4 entry, so the mapping is reachable under its CR3 too. */
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t* kp = vmm_kernel_pml4();
    uint64_t* ap = (uint64_t*)P2V(as.pml4_phys & PTE_PHYS_MASK);
    KEXPECT_EQ(ap[VMM_PML4_INDEX(va)], kp[VMM_PML4_INDEX(va)]);   /* slot 258 shared */
    KASSERT_TEST(ap[VMM_PML4_INDEX(va)] & PAGE_PRESENT);
    vmspace_destroy(&as);

    pmm_free_page(f);
}


/* Audit F3: two PT_LOAD segments whose pages overlap must not leak a frame.
 * The loader allocates a fresh frame per page and vmspace_map()s it; if a later
 * segment maps a page an earlier one already backed, the old frame must be
 * released, not orphaned. Oracle: create -> load -> destroy is net-zero pages.
 * The two segments here (vaddr 0x400000 and 0x400800) share page 0x400000. */
#ifndef O_RDWR
#define O_RDWR  0x0002
#endif
#ifndef O_CREAT
#define O_CREAT 0x0040
#endif
KTEST(vm, elf_overlapping_segments_no_frame_leak) {
    uint8_t elf[0x300];
    memset(elf, 0, sizeof elf);
    *(uint32_t*)(elf + 0)  = 0x464C457Fu;      /* \x7fELF           */
    elf[4] = 2; elf[5] = 1; elf[6] = 1;        /* ELFCLASS64, LE, v1 */
    *(uint16_t*)(elf + 16) = 2;                /* ET_EXEC            */
    *(uint16_t*)(elf + 18) = 0x3E;             /* x86-64             */
    *(uint64_t*)(elf + 24) = 0x400000ULL;      /* e_entry            */
    *(uint64_t*)(elf + 32) = 64;               /* e_phoff            */
    *(uint16_t*)(elf + 54) = 56;               /* e_phentsize        */
    *(uint16_t*)(elf + 56) = 2;                /* e_phnum            */
    uint8_t* p0 = elf + 64;
    *(uint32_t*)(p0 + 0)  = 1;                 /* PT_LOAD            */
    *(uint32_t*)(p0 + 4)  = 6;                 /* RW                 */
    *(uint64_t*)(p0 + 8)  = 0x200;             /* p_offset           */
    *(uint64_t*)(p0 + 16) = 0x400000ULL;       /* p_vaddr            */
    *(uint64_t*)(p0 + 32) = 0x40;              /* p_filesz           */
    *(uint64_t*)(p0 + 40) = 0x40;              /* p_memsz            */
    uint8_t* p1 = elf + 120;
    *(uint32_t*)(p1 + 0)  = 1;                 /* PT_LOAD            */
    *(uint32_t*)(p1 + 4)  = 6;                 /* RW                 */
    *(uint64_t*)(p1 + 8)  = 0x240;             /* p_offset           */
    *(uint64_t*)(p1 + 16) = 0x400800ULL;       /* same page 0x400000 */
    *(uint64_t*)(p1 + 32) = 0x40;              /* p_filesz           */
    *(uint64_t*)(p1 + 40) = 0x40;              /* p_memsz            */
    for (int i = 0; i < 0x40; i++) { elf[0x200 + i] = 0x11; elf[0x240 + i] = 0x22; }

    const char* path = "/elf_overlap";
    vfs_unlink(path);
    int fd = vfs_open(path, O_CREAT | O_RDWR);
    KASSERT_TEST(fd >= 0);
    KEXPECT_EQ((int)vfs_fd_write(fd, elf, 0x280), 0x280);
    vfs_close(fd);
    vnode_t* vn = vfs_resolve(path);
    KASSERT_TEST(vn != NULL);

    size_t snap = pmm_get_free_memory();
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t entry = 0;
    int r = elf_load(vn, &as, &entry, NULL);
    KEXPECT_EQ(r, 0);                               /* a valid (if odd) ELF loads */
    vmspace_destroy(&as);
    KEXPECT_EQ(pmm_get_free_memory(), snap);        /* no orphaned frame */
    vfs_unlink(path);
}


/* Audit F4: a fork that runs out of memory mid-copy must leak nothing. The
 * parent is mapped across two page tables (VAs 4 MiB apart -> two PD entries);
 * injection fails the allocation for the second PT, after the first PT and its
 * leaf's COW incref are already built. copy_level() must unwind that partial
 * subtree (free the tables, drop the leaf ref) so create->fork(fail)->destroy
 * returns every page. Runs IRQs-off so the fault lands on exactly the fork. */
KTEST(vm, fork_oom_rolls_back_cleanly) {
    address_space_t parent;
    KASSERT_TEST(vmspace_create(&parent) == 0);
    void* fa = pmm_alloc_page(); void* fb = pmm_alloc_page();
    KASSERT_TEST(fa && fb);
    page_setref((uint64_t)(uintptr_t)fa, 1);
    page_setref((uint64_t)(uintptr_t)fb, 1);
    KASSERT_TEST(vmspace_map(&parent, 0x400000, (uint64_t)(uintptr_t)fa, PAGE_WRITABLE) == 0);
    KASSERT_TEST(vmspace_map(&parent, 0x600000, (uint64_t)(uintptr_t)fb, PAGE_WRITABLE) == 0);

    size_t snap = pmm_get_free_memory();
    address_space_t child;
    irqflags_t fl = local_irq_save();
    pmm_test_fail_after(4);          /* child PML4, PDPT, PD, PT#1 ok; PT#2 fails */
    int r = vmspace_fork(&parent, &child);
    pmm_test_fail_after(-1);         /* disarm */
    local_irq_restore(fl);

    KEXPECT_EQ(r, -1);                               /* fork reported OOM        */
    KEXPECT_EQ(pmm_get_free_memory(), snap);         /* ...and leaked no page    */

    vmspace_destroy(&parent);
}
