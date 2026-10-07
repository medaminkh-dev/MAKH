/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - mm/uvm.h
 * User anonymous memory: brk and mmap/munmap/mprotect (Phase 20-B).
 *
 * These are the engines behind the brk/mmap/munmap/mprotect syscalls. They are
 * written as pure functions over an address space plus a pair of cursors (the
 * program break and the mmap bump pointer), so they can be exercised directly
 * in tests and by the fuzzer, independent of the process layer. The syscall
 * wrappers just pass the current process's address space and cursor fields.
 */

#ifndef MAKHOS_UVM_H
#define MAKHOS_UVM_H

#include <types.h>
#include <mm/vmspace.h>

/* Per-process anonymous-memory layout inside the private user window. The
 * program lives near the window base and the stack just above it; the heap and
 * the mmap arena sit well clear of both. */
#define USER_HEAP_BASE  0x0000200002000000ULL   /* brk region base (32 MiB in) */
#define USER_MMAP_BASE  0x0000200040000000ULL   /* mmap arena base (1 GiB in)   */
#define USER_HEAP_MAX   USER_MMAP_BASE           /* brk may not reach the arena  */
#define USER_ARENA_END  0x0000208000000000ULL    /* end of the PML4[64] window   */

/* brk(newbrk): move the program break. newbrk==0 queries the current break.
 * Returns the (new) break on success, the unchanged break on a rejected move. */
long uvm_brk(address_space_t* as, uint64_t* brk_cur, uint64_t brk_start,
             uint64_t newbrk);

/* mmap(len, prot, flags): anonymous, zero-filled, bump-allocated from *mmap_cur.
 * Only MAP_ANONYMOUS is supported. Returns the base address, or -errno. */
long uvm_mmap(address_space_t* as, uint64_t* mmap_cur, uint64_t len,
              int prot, int flags);

/* munmap(addr, len): unmap and free the range (holes are ignored). */
int  uvm_munmap(address_space_t* as, uint64_t addr, uint64_t len);

/* mprotect(addr, len, prot): change protection across the range. */
int  uvm_mprotect(address_space_t* as, uint64_t addr, uint64_t len, int prot);

#endif /* MAKHOS_UVM_H */
