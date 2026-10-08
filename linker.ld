/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - linker.ld
 * Higher-half kernel (F21 Path A). The 32-bit boot trampoline runs with paging
 * off, so it is linked 1:1 at its physical load address (0x100000) in a low
 * .boot section. Everything else is linked at KERNEL_VMA + physical (the
 * -2 GiB "kernel" region, -mcmodel=kernel) but still LOADED low via AT(), so
 * GRUB drops the whole image at 0x100000 and the trampoline jumps up into the
 * higher half once paging is on.
 */

ENTRY(start_32bit)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Physical load address at the 1MB mark. */
    . = 0x100000;

    /* Low boot trampoline: multiboot header + the 32-bit entry and the 64-bit
     * low stub that jumps into the higher half. VMA == LMA == physical, because
     * this code executes before (and during) the switch into long mode. */
    .boot ALIGN(8) :
    {
        *(.multiboot)
        *(.boot)
        *(.boot.*)
    }

    /* Shift the link cursor into the higher half; the rest of the kernel is
     * addressed there at runtime but AT()-loaded at its low physical. */
    . = ALIGN(4K);
    . += KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata)
        *(.rodata.*)
    }

    /* In-kernel test registry (see ktest.h). Runner walks start..end. */
    .ktests ALIGN(8) : AT(ADDR(.ktests) - KERNEL_VMA)
    {
        __ktests_start = .;
        KEEP(*(.ktests))
        __ktests_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    /* Kernel end symbol (higher-half VMA) for memory management / symbolization. */
    KERNEL_END = .;

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.gnu.hash)
    }
}
