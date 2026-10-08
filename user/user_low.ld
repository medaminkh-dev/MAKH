/* SPDX-License-Identifier: AGPL-3.0-only */
/* Link a user program at a conventional low address (0x400000, PML4 slot 0),
 * the way a standard non-PIE toolchain (gcc/tcc) lays out an ET_EXEC. Since the
 * higher-half kernel migration (F21 Path A) the whole lower canonical half
 * belongs to the process, so this loads and runs like any other user program.
 * Two segments keep W^X (code R+X, data/bss R+W), same as user.ld. */
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}
SECTIONS {
    . = 0x0000000000400000;
    .text   : { *(.text*) }            :text
    .rodata : { *(.rodata*) }          :text
    . = ALIGN(0x1000);
    .data   : { *(.data*) }            :data
    .bss    : { *(.bss*) *(COMMON) }   :data
}
