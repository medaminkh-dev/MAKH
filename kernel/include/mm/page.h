/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/page.h
 * Per-frame reference counts (Phase 17).
 *
 * Copy-on-write means one physical frame can be mapped by several address
 * spaces at once. A refcount per frame decides when a frame is really free:
 * fork increments it, a COW copy or an address-space teardown decrements it,
 * and the frame returns to the PMM only when the count reaches zero.
 *
 * The array is sized to actual RAM and lives in the kernel heap, so it costs
 * ~2 bytes per 4 KiB of RAM and nothing for memory that does not exist.
 */

#ifndef MAKHOS_PAGE_H
#define MAKHOS_PAGE_H

#include <types.h>

/* Allocate and zero the refcount array. Call after kheap_init(). */
void page_init(void);

/* Set / read a frame's refcount (phys is a physical address; low bits ignored). */
void     page_setref(uint64_t phys, uint32_t count);
uint32_t page_refcount(uint64_t phys);

/* ++refcount. */
void page_incref(uint64_t phys);

/* --refcount; when it reaches zero the frame is returned to the PMM.
 * Returns the new count. */
uint32_t page_decref(uint64_t phys);

#endif /* MAKHOS_PAGE_H */
