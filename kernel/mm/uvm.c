/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/uvm.c
 * User anonymous memory: brk and mmap/munmap/mprotect (Phase 20-B). See uvm.h.
 *
 * Anonymous memory is zero-filled on allocation. mmap is a bump allocator over
 * the mmap arena: freed ranges are unmapped (their frames returned) but their
 * addresses are not recycled — simple, and the 512 GiB window makes exhaustion
 * a non-issue for now. Address reuse and file-backed mappings come later.
 */

#include <mm/uvm.h>
#include <mm/vmspace.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <lib/string.h>
#include <syscall.h>
#include <errno.h>

#define PAGE_UP(x)   (((x) + 0xFFFULL) & ~0xFFFULL)

/* PROT_* bits -> leaf PTE flags. A page is non-executable unless PROT_EXEC is
 * asked for, which keeps anonymous memory W^X-friendly by default. */
static uint64_t prot_to_flags(int prot) {
    uint64_t f = 0;
    if (prot & PROT_WRITE) f |= PAGE_WRITABLE;
    if (!(prot & PROT_EXEC)) f |= PAGE_NO_EXECUTE;
    return f;
}

/* Map [va, va+len) with fresh zeroed frames. On failure, roll back every page
 * already mapped so the call is all-or-nothing. len is a page multiple. */
static int map_anon(address_space_t* as, uint64_t va, uint64_t len, uint64_t flags) {
    for (uint64_t off = 0; off < len; off += 4096) {
        void* fp = pmm_alloc_page();
        if (!fp) goto rollback;
        uint64_t pf = (uint64_t)(uintptr_t)fp;
        memset(P2V(pf), 0, 4096);               /* zero via HHDM (any CR3) */
        page_setref(pf, 1);
        if (vmspace_map(as, va + off, pf, flags) != 0) {
            page_decref(pf);                     /* unmapped frame: free it */
            goto rollback;
        }
        continue;
    rollback:
        for (uint64_t u = 0; u < off; u += 4096) vmspace_unmap(as, va + u);
        return -1;
    }
    return 0;
}

long uvm_brk(address_space_t* as, uint64_t* brk_cur, uint64_t brk_start,
             uint64_t newbrk) {
    if (newbrk == 0) return (long)*brk_cur;              /* query */
    if (newbrk < brk_start || newbrk > USER_HEAP_MAX)
        return (long)*brk_cur;                           /* rejected: unchanged */

    uint64_t have = PAGE_UP(*brk_cur);                   /* pages currently mapped */
    uint64_t want = PAGE_UP(newbrk);

    if (want > have) {                                   /* grow */
        if (map_anon(as, have, want - have, PAGE_WRITABLE | PAGE_NO_EXECUTE) != 0)
            return (long)*brk_cur;                       /* OOM: leave break put */
    } else {                                             /* shrink (or no change) */
        for (uint64_t va = want; va < have; va += 4096) vmspace_unmap(as, va);
    }
    *brk_cur = newbrk;
    return (long)newbrk;
}

long uvm_mmap(address_space_t* as, uint64_t* mmap_cur, uint64_t len,
              int prot, int flags) {
    if (len == 0) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS)) return -EINVAL;        /* anon only for now */
    len = PAGE_UP(len);

    uint64_t va = *mmap_cur;
    if (va < USER_MMAP_BASE) va = USER_MMAP_BASE;
    if (va + len > USER_ARENA_END || va + len < va) return -ENOMEM;

    if (map_anon(as, va, len, prot_to_flags(prot)) != 0) return -ENOMEM;
    *mmap_cur = va + len;
    return (long)va;
}

int uvm_munmap(address_space_t* as, uint64_t addr, uint64_t len) {
    if (addr & 0xFFF) return -EINVAL;
    len = PAGE_UP(len);
    for (uint64_t off = 0; off < len; off += 4096)
        vmspace_unmap(as, addr + off);                   /* holes are fine */
    return 0;
}

int uvm_mprotect(address_space_t* as, uint64_t addr, uint64_t len, int prot) {
    if (addr & 0xFFF) return -EINVAL;
    len = PAGE_UP(len);
    for (uint64_t off = 0; off < len; off += 4096)
        if (vmspace_protect(as, addr + off, prot_to_flags(prot)) != 0)
            return -ENOMEM;                              /* an unmapped page */
    return 0;
}
