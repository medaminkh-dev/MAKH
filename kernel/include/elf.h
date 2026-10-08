/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - elf.h
 * Minimal ELF64 loader (Phase 20-A; Phase 20-O adds static-PIE / ET_DYN).
 */
#ifndef MAKHOS_ELF_H
#define MAKHOS_ELF_H

#include <types.h>

struct vnode;
struct address_space;

/*
 * What the ELF loader learned about the image, for the SysV auxiliary vector a
 * C runtime reads at startup (Phase 20-O). A static-PIE (ET_DYN) musl binary
 * self-relocates using AT_PHDR to find its own program headers and PT_DYNAMIC,
 * and AT_PHDR/AT_PHENT/AT_PHNUM to locate its PT_TLS; AT_BASE is 0 (no interp).
 */
typedef struct {
    uint64_t at_phdr;    /* user VA of the program headers in the loaded image */
    uint64_t at_phent;   /* e_phentsize (one program-header entry)             */
    uint64_t at_phnum;   /* e_phnum                                            */
    uint64_t at_base;    /* load bias applied to a PIE (0 for a fixed ET_EXEC) */
} elf_aux_t;

/*
 * Load an ELF64 program from `file` into the address space `as`, mapping each
 * PT_LOAD segment with W^X (code RX, data RW+NX). Two kinds are accepted:
 *
 *   - ET_EXEC — a fixed-address executable, linked within the per-process user
 *     region (PML4 slot 64, VAs at or above 0x0000_2000_0000_0000);
 *   - ET_DYN  — a position-independent executable (static-PIE), loaded at a
 *     fixed bias inside that region; every p_vaddr is interpreted relative to
 *     the bias and the binary relocates itself at startup.
 *
 * Writes the (bias-adjusted) entry point to *entry. If `aux` is non-NULL it is
 * filled with the auxiliary-vector facts above. Returns 0, or -errno.
 */
int elf_load(struct vnode* file, struct address_space* as, uint64_t* entry,
             elf_aux_t* aux);

#endif /* MAKHOS_ELF_H */
