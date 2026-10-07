/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - proc/elf.c
 * Minimal static ELF64 loader. See elf.h.
 *
 * Segments are loaded into the target address space by allocating fresh frames,
 * copying file bytes in through the identity map (so the loader does not need
 * the address space to be active), and mapping them with W^X. Frame refcounts
 * are set to 1 so vmspace_destroy() reclaims them.
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
#define PT_LOAD   1
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

int elf_load(vnode_t* file, address_space_t* as, uint64_t* entry) {
    elf64_ehdr_t eh;
    if (vfs_read(file, &eh, sizeof(eh), 0) != (long)sizeof(eh)) return -EIO;
    if (eh.e_magic != ELF_MAGIC || eh.e_class != 2 /*ELFCLASS64*/) return -ENOEXEC;
    if (eh.e_type != ET_EXEC) return -ENOEXEC;
    if (eh.e_phnum == 0 || eh.e_phentsize != sizeof(elf64_phdr_t)) return -ENOEXEC;

    for (uint16_t i = 0; i < eh.e_phnum; i++) {
        elf64_phdr_t ph;
        uint64_t off = eh.e_phoff + (uint64_t)i * sizeof(ph);
        if (vfs_read(file, &ph, sizeof(ph), off) != (long)sizeof(ph)) return -EIO;
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;

        /* The segment must lie wholly inside the private per-process window.
         * Checked overflow-safe: reject a vaddr below the window, a size wider
         * than the window, or an end that would spill past it (which could be
         * a wrap to a low address or a reach into the kernel's PML4 slots). */
        if (ph.p_vaddr < USER_VA_MIN) return -ENOEXEC;
        if (ph.p_memsz > USER_SEG_MAX) return -ENOMEM;   /* bounded work */
        if (ph.p_vaddr > USER_VA_END - ph.p_memsz) return -ENOEXEC;
        /* A segment whose file image is larger than its memory image is
         * malformed; cap the copy at p_memsz so we never read past the frames
         * we allocate for it. */
        if (ph.p_filesz > ph.p_memsz) return -ENOEXEC;

        uint64_t flags = PAGE_WRITABLE;                 /* writable while we fill it */
        uint64_t va_start = ph.p_vaddr & ~0xFFFULL;
        uint64_t va_end   = (ph.p_vaddr + ph.p_memsz + 0xFFF) & ~0xFFFULL;

        for (uint64_t va = va_start; va < va_end; va += 4096) {
            void* f = pmm_alloc_page();
            if (!f) return -ENOMEM;
            memset(f, 0, 4096);
            uint64_t pf = (uint64_t)(uintptr_t)f;
            page_setref(pf, 1);

            /* Copy the overlapping slice of this page's file content. */
            uint64_t pstart = ph.p_vaddr;
            uint64_t pfile_end = ph.p_vaddr + ph.p_filesz;
            uint64_t copy_from = va > pstart ? va : pstart;
            uint64_t copy_to   = (va + 4096) < pfile_end ? (va + 4096) : pfile_end;
            if (copy_to > copy_from) {
                uint64_t foff = ph.p_offset + (copy_from - pstart);
                vfs_read(file, (void*)(uintptr_t)(pf + (copy_from - va)),
                         (size_t)(copy_to - copy_from), foff);
            }
            if (vmspace_map(as, va, pf, flags) != 0) return -ENOMEM;
        }

        /* Re-map with the segment's real protection (W^X): an executable
         * segment becomes read-only + executable; a writable segment RW + NX;
         * read-only data NX and not writable. */
        uint64_t prot = (ph.p_flags & PF_X) ? 0
                      : (ph.p_flags & PF_W) ? (PAGE_WRITABLE | PAGE_NO_EXECUTE)
                                            : PAGE_NO_EXECUTE;
        for (uint64_t va = va_start; va < va_end; va += 4096)
            vmspace_map(as, va, vmspace_phys(as, va), prot);
    }

    *entry = eh.e_entry;
    KLOG_I("ELF", "loaded entry=%p\n", (void*)eh.e_entry);
    return 0;
}
