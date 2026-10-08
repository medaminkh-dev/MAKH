/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - proc/elf.c
 * Minimal ELF64 loader. See elf.h.
 *
 * Segments are loaded into the target address space by allocating fresh frames,
 * copying file bytes in through the identity map (so the loader does not need
 * the address space to be active), and mapping them with W^X. Frame refcounts
 * are set to 1 so vmspace_destroy() reclaims them.
 *
 * Two program kinds are accepted (Phase 20-O):
 *   - ET_EXEC: a fixed-address executable (bias 0); its p_vaddr already lie in
 *     the per-process window;
 *   - ET_DYN: a static-PIE, loaded at a fixed bias (USER_PIE_BASE) inside the
 *     window. A PIE's p_vaddr are relative to 0, so every address — segments,
 *     the entry, the program headers we hand back for AT_PHDR — is biased. The
 *     binary carries only R_X86_64_RELATIVE relocations and fixes itself up at
 *     startup (musl's rcrt1), so the loader applies no relocations.
 */

#include <elf.h>
#include <fs/vfs.h>
#include <mm/vmspace.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <lib/string.h>
#include <errno.h>
#include <klog.h>

#define ELF_MAGIC 0x464C457Fu   /* "\x7fELF" little-endian */
#define ET_EXEC   2
#define ET_DYN    3
#define PT_LOAD   1
#define PT_PHDR   6
#define PF_X      1
#define PF_W      2

/* The private per-process window is exactly one PML4 slot (index 64):
 * [USER_VA_MIN, USER_VA_END). A PT_LOAD segment must lie wholly inside it, or
 * vmspace_map() would walk into the shared kernel PML4 entries and corrupt
 * them. Everything an untrusted ELF asks for is validated against this range
 * with overflow-safe arithmetic (a malformed ELF is the common case the KFUZZ
 * "elf" target hammers). */
#define USER_VA_MIN  0x0000200000000000ULL   /* 64 << 39 */
#define USER_VA_END  0x0000208000000000ULL   /* 65 << 39 */

/* A static-PIE loads at the bottom of the window. Its image is a handful of
 * pages; the user stack sits 8 MiB in, the heap 32 MiB in, and the mmap arena
 * 1 GiB in (mm/uvm.h), so the bias leaves generous room below all of them. A
 * program "NULL" stays the unmapped virtual 0, so a null deref still faults. */
#define USER_PIE_BASE  USER_VA_MIN

/* Ceiling on a single PT_LOAD segment. Without it a malformed (or merely
 * greedy) p_memsz that still fits the window drives elf_load into a
 * multi-million-page allocation loop — a denial of service that the KFUZZ
 * "elf" target reliably trips. Our programs are a few pages; 16 MiB is
 * generous headroom and bumps easily when real binaries need it. */
#define USER_SEG_MAX  (16u * 1024u * 1024u)

typedef struct {
    uint32_t e_magic;
    uint8_t  e_class, e_data, e_version, e_osabi, e_pad[8];
    uint16_t e_type, e_machine;
    uint32_t e_version2;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed)) elf64_phdr_t;

int elf_load(vnode_t* file, address_space_t* as, uint64_t* entry, elf_aux_t* aux) {
    elf64_ehdr_t eh;
    if (vfs_read(file, &eh, sizeof(eh), 0) != (long)sizeof(eh)) return -EIO;
    if (eh.e_magic != ELF_MAGIC || eh.e_class != 2 /*ELFCLASS64*/) return -ENOEXEC;
    if (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) return -ENOEXEC;
    if (eh.e_phnum == 0 || eh.e_phentsize != sizeof(elf64_phdr_t)) return -ENOEXEC;

    /* A PIE floats; a fixed executable is already placed. */
    uint64_t bias = (eh.e_type == ET_DYN) ? USER_PIE_BASE : 0;

    /* Program-header address for AT_PHDR: prefer the PT_PHDR entry, else the
     * file offset of the header table mapped through the LOAD segment that
     * contains it. Both are discovered while we walk the headers below. */
    uint64_t phdr_va = 0;              /* from PT_PHDR (p_vaddr) */
    uint64_t phdr_va_fallback = 0;     /* from the LOAD covering e_phoff */

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t ph;
        uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(ph);
        if (vfs_read(file, &ph, sizeof(ph), off) != (long)sizeof(ph)) return -EIO;

        if (ph.p_type == PT_PHDR) { phdr_va = bias + ph.p_vaddr; continue; }
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        /* The segment must lie wholly inside the private per-process window
         * after biasing. Checked overflow-safe: reject a biased start that
         * wrapped or fell below the window, a size wider than the ceiling, or
         * an end that would spill past the window (a reach into the kernel's
         * PML4 slots). */
        if (ph.p_memsz > USER_SEG_MAX) return -ENOMEM;   /* bounded work */
        if (ph.p_vaddr > USER_VA_END) return -ENOEXEC;   /* absurd offset */
        uint64_t seg = bias + ph.p_vaddr;
        if (seg < bias) return -ENOEXEC;                 /* bias+vaddr wrapped */
        if (seg < USER_VA_MIN) return -ENOEXEC;
        if (seg > USER_VA_END - ph.p_memsz) return -ENOEXEC;
        /* A segment whose file image is larger than its memory image is
         * malformed; cap the copy at p_memsz so we never read past the frames
         * we allocate for it. */
        if (ph.p_filesz > ph.p_memsz) return -ENOEXEC;

        /* Does the program-header table live inside this segment's file image?
         * Keep it as the AT_PHDR fallback when there is no PT_PHDR entry. */
        if (eh.e_phoff >= ph.p_offset &&
            eh.e_phoff <  ph.p_offset + ph.p_filesz)
            phdr_va_fallback = seg + (eh.e_phoff - ph.p_offset);

        uint64_t flags = PAGE_WRITABLE;                 /* writable while we fill it */
        uint64_t va_start = seg & ~0xFFFULL;
        uint64_t va_end   = (seg + ph.p_memsz + 0xFFF) & ~0xFFFULL;

        for (uint64_t va = va_start; va < va_end; va += 4096) {
            void* f = pmm_alloc_page();
            if (!f) return -ENOMEM;
            uint64_t pf = (uint64_t)(uintptr_t)f;
            uint8_t* fv = (uint8_t*)P2V(pf);     /* CPU view via HHDM (any CR3) */
            memset(fv, 0, 4096);
            page_setref(pf, 1);

            /* Copy the overlapping slice of this page's file content. */
            uint64_t pstart = seg;
            uint64_t pfile_end = seg + ph.p_filesz;
            uint64_t copy_from = va > pstart ? va : pstart;
            uint64_t copy_to   = (va + 4096) < pfile_end ? (va + 4096) : pfile_end;
            if (copy_to > copy_from) {
                uint64_t foff = ph.p_offset + (copy_from - pstart);
                vfs_read(file, fv + (copy_from - va),
                         (size_t)(copy_to - copy_from), foff);
            }
            if (vmspace_map(as, va, pf, flags) != 0) return -ENOMEM;
        }

        /* Re-map with the segment's real protection (W^X): an executable
         * segment becomes read-only + executable; a writable segment RW + NX;
         * read-only data NX and not writable. A PIE's relocations and RELRO
         * all target writable segments, so self-relocation stays within W^X. */
        uint64_t prot = (ph.p_flags & PF_X) ? 0
                      : (ph.p_flags & PF_W) ? (PAGE_WRITABLE | PAGE_NO_EXECUTE)
                                            : PAGE_NO_EXECUTE;
        for (uint64_t va = va_start; va < va_end; va += 4096)
            vmspace_map(as, va, vmspace_phys(as, va), prot);
    }

    *entry = bias + eh.e_entry;
    if (aux) {
        aux->at_phdr = phdr_va ? phdr_va : phdr_va_fallback;
        aux->at_phent = eh.e_phentsize;
        aux->at_phnum = eh.e_phnum;
        aux->at_base = bias;
    }
    KLOG_I("ELF", "loaded %s entry=%p bias=%p\n",
           eh.e_type == ET_DYN ? "PIE" : "EXEC",
           (void*)(uintptr_t)*entry, (void*)(uintptr_t)bias);
    return 0;
}
