/* SPDX-License-Identifier: AGPL-3.0-only */
/* Link user programs inside the per-process region (PML4 slot 64). */
ENTRY(_start)
SECTIONS {
    . = 0x0000200000400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
