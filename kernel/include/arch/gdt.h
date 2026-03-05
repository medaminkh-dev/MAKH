#ifndef MAKHOS_GDT_H
#define MAKHOS_GDT_H

#include <types.h>
#include <arch/tss.h>

#define GDT_NULL        0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

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
