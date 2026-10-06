/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/page.c
 * Per-frame reference counts for copy-on-write. See mm/page.h.
 */

#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <irq.h>
#include <klog.h>

static uint16_t* refcount;     /* indexed by physical frame number (phys >> 12) */
static uint64_t  nframes;

void page_init(void) {
    nframes = pmm_get_total_page_count();
    refcount = kcalloc(nframes, sizeof(uint16_t));
    if (!refcount) {
        KLOG_E("PAGE", "could not allocate refcount array for %lu frames\n",
               (unsigned long)nframes);
        return;
    }
    KLOG_I("PAGE", "refcounts for %lu frames (%lu KiB)\n",
           (unsigned long)nframes, (unsigned long)(nframes * sizeof(uint16_t) / 1024));
}

static inline uint64_t pfn(uint64_t phys) { return phys >> 12; }

void page_setref(uint64_t phys, uint32_t count) {
    uint64_t i = pfn(phys);
    if (!refcount || i >= nframes) return;
    irqflags_t f = local_irq_save();
    refcount[i] = (uint16_t)(count > 0xFFFF ? 0xFFFF : count);
    local_irq_restore(f);
}

uint32_t page_refcount(uint64_t phys) {
    uint64_t i = pfn(phys);
    if (!refcount || i >= nframes) return 0;
    return refcount[i];
}

void page_incref(uint64_t phys) {
    uint64_t i = pfn(phys);
    if (!refcount || i >= nframes) return;
    irqflags_t f = local_irq_save();
    if (refcount[i] < 0xFFFF) refcount[i]++;
    local_irq_restore(f);
}

uint32_t page_decref(uint64_t phys) {
    uint64_t i = pfn(phys);
    if (!refcount || i >= nframes) return 0;
    irqflags_t f = local_irq_save();
    uint32_t n = refcount[i];
    if (n > 0) {
        n--;
        refcount[i] = (uint16_t)n;
    }
    local_irq_restore(f);
    if (n == 0) pmm_free_page((void*)(uintptr_t)(phys & ~0xFFFULL));
    return n;
}
