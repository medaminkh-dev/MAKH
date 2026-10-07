/* SPDX-License-Identifier: AGPL-3.0-only */
/* Link user programs inside the per-process region (PML4 slot 64).
 * Two segments so W^X holds: code is R+X, data/bss are R+W (non-exec). The
 * page-aligned split lets the ELF loader map each with the right protection,
 * and makes .data genuinely writable (and copy-on-write across fork). */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}
SECTIONS {
    . = 0x0000200000400000;
    .text   : { *(.text*) }            :text
    .rodata : { *(.rodata*) }          :text
    . = ALIGN(0x1000);
    .data   : { *(.data*) }            :data
    .bss    : { *(.bss*) *(COMMON) }   :data
}
