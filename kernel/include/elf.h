/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - elf.h
 * Minimal static ELF64 loader (Phase 20-A).
 */
#ifndef MAKHOS_ELF_H
#define MAKHOS_ELF_H

#include <types.h>

struct vnode;
struct address_space;

/*
 * Load a static ET_EXEC ELF64 from `file` into the address space `as`,
 * mapping each PT_LOAD segment with W^X (code RX, data RW+NX). The program
 * must be linked within the per-process user region (PML4 slot 64, i.e. VAs
 * at or above 0x0000_2000_0000_0000). Writes the entry point to *entry.
 * Returns 0 on success, -errno on failure.
 */
int elf_load(struct vnode* file, struct address_space* as, uint64_t* entry);

#endif /* MAKHOS_ELF_H */
