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
