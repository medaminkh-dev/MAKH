/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/vmspace.h
 * Per-process virtual address spaces with copy-on-write (Phase 17).
 *
 * An address space is one PML4. Every space shares the kernel's PML4 entries
 * by value, so the kernel half (code, heap, identity map) stays mapped under
 * any CR3; only the user region (one PML4 slot) is private per space.
 *
 * fork() duplicates the user region lazily: the page-table structure is copied
 * but the data frames are shared read-only and marked copy-on-write, their
 * refcounts bumped. The first write faults, and vmspace_cow_fault() gives the
 * writer a private copy (or simply restores write permission if it is now the
 * sole owner).
 *
 * This is the VMM-level machinery. Wiring it to a process-level fork() syscall
 * with two scheduled user processes is Phase 18; here it is exercised directly.
 */

#ifndef MAKHOS_VMSPACE_H
#define MAKHOS_VMSPACE_H

#include <types.h>

/* The single PML4 slot that holds a space's private user mappings. Matches the
 * Phase 16 user window (USER_CODE_BASE = 0x0000_2000_0000_0000). */
#define USER_PML4_INDEX  64

typedef struct address_space {
    uint64_t pml4_phys;      /* physical address of this space's PML4 */
    uint32_t refcount;       /* threads sharing this space (clone CLONE_VM) */
} address_space_t;

/* Create a fresh space: a new PML4 sharing all kernel entries, empty user
 * region. Returns 0 on success, -1 on OOM. */
int  vmspace_create(address_space_t* as);

/* Tear a space down: drop a reference to every user data frame (freeing those
 * that reach zero), free the user page tables and the PML4. Shared kernel
 * tables are never touched. */
void vmspace_destroy(address_space_t* as);

/* Map one user frame into a space (sets PAGE_USER on the whole walk). `flags`
 * are the leaf flags (PAGE_WRITABLE, PAGE_NO_EXECUTE, ...); PRESENT|USER are
 * added. The caller owns the frame's refcount. */
int  vmspace_map(address_space_t* as, uint64_t va, uint64_t phys, uint64_t flags);

/* Physical frame backing `va`, or 0 if unmapped. */
uint64_t vmspace_phys(address_space_t* as, uint64_t va);

/* Drop the mapping at `va`, releasing its frame. 0 if unmapped, -1 if none. */
int  vmspace_unmap(address_space_t* as, uint64_t va);

/* Change the leaf protection (PAGE_WRITABLE / PAGE_NO_EXECUTE) at `va`, keeping
 * the frame. COW pages stay COW. 0 on success, -1 if `va` is unmapped. */
int  vmspace_protect(address_space_t* as, uint64_t va, uint64_t flags);

/* COW-fork `parent` into `child` (which must be uninitialised): the user
 * region becomes shared copy-on-write in both. Returns 0 or -1. */
int  vmspace_fork(address_space_t* parent, address_space_t* child);

/* Resolve a copy-on-write write fault at `va`: give the space a private,
 * writable copy of the frame (or restore write if it is the sole owner).
 * Returns 0 if it handled a COW page, -1 otherwise. */
int  vmspace_cow_fault(address_space_t* as, uint64_t va);

/* Make `as` the active address space (load CR3). */
void vmspace_switch(address_space_t* as);

#endif /* MAKHOS_VMSPACE_H */
