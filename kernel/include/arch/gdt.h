/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_GDT_H
#define MAKHOS_GDT_H

#include <types.h>
#include <arch/tss.h>

#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
/* User DATA precedes user CODE: `sysretq` derives SS from STAR[63:48]+8 and
 * CS from STAR[63:48]+16, so the data selector must sit 8 bytes below the
 * 64-bit code selector. STAR[63:48] = 0x10 then gives SS=0x18, CS=0x20. */
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* Ring-3 selectors carry RPL 3. */
#define SEL_USER_CODE   (GDT_USER_CODE | 3)   /* 0x23 */
#define SEL_USER_DATA   (GDT_USER_DATA | 3)   /* 0x1B */

#define GDT_ENTRIES     7    // 6 regular + 2 for 128-bit TSS descriptor (entries 5&6)

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_ptr_t;

void gdt_init(void);
void gdt_load_tss(void);
void gdt_reload_segments(void);

#endif
