/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_mm.c
 * Tests for the physical memory manager and kernel heap.
 */

#include <ktest.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <mm/vmm.h>
#include <lib/string.h>
#include <irq.h>

/* -------------------------------------------------------------------------- */
/* Physical memory manager                                                    */
/* -------------------------------------------------------------------------- */

KTEST(pmm, alloc_is_page_aligned_and_unique) {
    void* pages[16];
    for (int i = 0; i < 16; i++) {
        pages[i] = pmm_alloc_page();
        KASSERT_TEST(pages[i] != (void*)0);
        KEXPECT_EQ((uintptr_t)pages[i] & (PAGE_SIZE - 1), 0);
        /* No page returned twice while all are held. */
        for (int j = 0; j < i; j++) KEXPECT_NE((uintptr_t)pages[i], (uintptr_t)pages[j]);
    }
    for (int i = 0; i < 16; i++) pmm_free_page(pages[i]);
}

KTEST(pmm, free_then_realloc_conserves_count) {
    uint32_t used_before = pmm_get_used_page_count();
    void* p = pmm_alloc_page();
    KASSERT_TEST(p != (void*)0);
    KEXPECT_EQ(pmm_get_used_page_count(), used_before + 1);
    pmm_free_page(p);
    KEXPECT_EQ(pmm_get_used_page_count(), used_before);
}

KTEST(pmm, allocated_frame_is_writable) {
    /* PMM frames are identity-mapped, so we can write through the address. */
    volatile uint64_t* p = (volatile uint64_t*)pmm_alloc_page();
    KASSERT_TEST(p != (void*)0);
    p[0] = 0xCAFEBABEULL;
    p[511] = 0x1234ULL;  /* last qword of the 4KB frame */
    KEXPECT_EQ(p[0], 0xCAFEBABEULL);
    KEXPECT_EQ(p[511], 0x1234ULL);
    pmm_free_page((void*)p);
}

/* -------------------------------------------------------------------------- */
/* Higher-half direct map (HHDM) — F21 Path A groundwork                       */
/* -------------------------------------------------------------------------- */

/* P2V(phys) must address the very same physical frame as an independent
 * mapping of that frame, and must land in the higher half (never equal the
 * physical address). Proven without leaning on the legacy low identity map, so
 * the invariant keeps holding once that map is retired from user spaces. */
KTEST(vmm, hhdm_aliases_physical_frame) {
    void* phys = pmm_alloc_page();
    KASSERT_TEST(phys != (void*)0);
    uint64_t pa = (uint64_t)(uintptr_t)phys;

    volatile uint64_t* via_hhdm = (volatile uint64_t*)P2V(pa);
    KEXPECT_NE((uint64_t)(uintptr_t)via_hhdm, pa);       /* higher half, != phys */

    /* An independent scratch mapping of the same frame (slot 260). */
    uint64_t scratch = 0xFFFF820000000000ULL;
    KASSERT_TEST(vmm_map_page(scratch, pa, PAGE_PRESENT | PAGE_WRITABLE) == 0);
    volatile uint64_t* via_scratch = (volatile uint64_t*)(uintptr_t)scratch;

    *via_hhdm = 0xCAFEBABE12345678ULL;                   /* write via HHDM ... */
    KEXPECT_EQ(*via_scratch, 0xCAFEBABE12345678ULL);     /* ... seen via scratch */
    *via_scratch = 0x0123456789ABCDEFULL;                /* write via scratch ... */
    KEXPECT_EQ(*via_hhdm, 0x0123456789ABCDEFULL);        /* ... seen via HHDM */

    vmm_unmap_page(scratch);
    pmm_free_page(phys);
}

/* -------------------------------------------------------------------------- */
/* Kernel heap                                                                */
/* -------------------------------------------------------------------------- */

KTEST(kheap, returns_16byte_aligned) {
    size_t sizes[] = {1, 7, 8, 15, 16, 17, 64, 100, 256, 1000, 4096};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void* p = kmalloc(sizes[i]);
        KASSERT_TEST(p != (void*)0);
        KEXPECT_EQ((uintptr_t)p & 15, 0);
        kfree(p);
    }
}

KTEST(kheap, write_read_does_not_corrupt_neighbours) {
    /* Allocate a row of blocks, fill each with a distinct pattern, verify. */
    enum { N = 32 };
    uint8_t* blocks[N];
    for (int i = 0; i < N; i++) {
        blocks[i] = (uint8_t*)kmalloc(128);
        KASSERT_TEST(blocks[i] != (void*)0);
        memset(blocks[i], i + 1, 128);
    }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < 128; j++)
            KEXPECT_EQ(blocks[i][j], (uint8_t)(i + 1));
    for (int i = 0; i < N; i++) kfree(blocks[i]);
}

KTEST(kheap, kcalloc_zeroes) {
    int* a = (int*)kcalloc(256, sizeof(int));
    KASSERT_TEST(a != (void*)0);
    for (int i = 0; i < 256; i++) KEXPECT_EQ(a[i], 0);
    kfree(a);
}

KTEST(kheap, free_and_coalesce_reclaims_space) {
    /* Churn: repeated alloc/free of growing sizes must not run the heap dry
     * (coalescing should recycle freed blocks). */
    for (int round = 0; round < 200; round++) {
        void* p = kmalloc(512);
        KASSERT_TEST(p != (void*)0);
        memset(p, 0xEE, 512);
        kfree(p);
    }
    /* A large alloc should still succeed afterwards. */
    void* big = kmalloc(8192);
    KEXPECT(big != (void*)0);
    kfree(big);
}

KTEST(kheap, krealloc_preserves_prefix) {
    char* s = (char*)kmalloc(16);
    KASSERT_TEST(s != (void*)0);
    for (int i = 0; i < 16; i++) s[i] = (char)('A' + (i % 26));
    char* g = (char*)krealloc(s, 128);
    KASSERT_TEST(g != (void*)0);
    for (int i = 0; i < 16; i++) KEXPECT_EQ(g[i], (char)('A' + (i % 26)));
    kfree(g);
}

/* kheap_check() is the heap oracle used by the self-fuzzer: it must pass on
 * a healthy heap after heavy churn, and must notice a smashed footer. */
KTEST(kheap, integrity_walker_passes_after_churn) {
    void* p[64];
    uint32_t x = 0x1234567u;
    for (int i = 0; i < 64; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = kmalloc(16 + (x >> 20) % 3000);
        KASSERT_TEST(p[i] != NULL);
    }
    for (int i = 0; i < 64; i += 2) kfree(p[i]);
    KEXPECT_EQ(kheap_check(), 0);
    for (int i = 1; i < 64; i += 2) kfree(p[i]);
    KEXPECT_EQ(kheap_check(), 0);
}

KTEST(kheap, integrity_walker_detects_smashed_footer) {
    uint8_t* p = kmalloc(40);
    KASSERT_TEST(p != NULL);
    /* The block is 32 (hdr) + 48 (payload) + 16 (footer): the footer magic
     * starts right after the 48-byte aligned payload. */
    volatile uint32_t* footer_magic = (volatile uint32_t*)(p + 48);
    irqflags_t f = local_irq_save();            /* nobody else may see it */
    uint32_t saved = *footer_magic;
    *footer_magic = 0x41414141;                 /* simulated overrun */
    int smashed = kheap_check();
    uint64_t bad = kheap_check_bad_block();
    *footer_magic = saved;
    local_irq_restore(f);
    KEXPECT(smashed != 0);
    KEXPECT(bad == (uint64_t)(uintptr_t)(p - 32));
    KEXPECT_EQ(kheap_check(), 0);
    kfree(p);
}
