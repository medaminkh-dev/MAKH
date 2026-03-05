#include <arch/gdt.h>
#include <arch/tss.h>
#include <lib/string.h>
#include <vga.h>
#include <kernel.h>

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t gdt_ptr;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, 
                          uint8_t access, uint8_t gran) {
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].access = access;
    gdt[index].granularity = (limit >> 16) & 0x0F;
    gdt[index].granularity |= gran & 0xF0;
    gdt[index].base_high = (base >> 24) & 0xFF;
}

// TSS descriptor is 128 bits in x86_64 (uses 2 GDT entries)
static void gdt_set_tss_entry(int index, uint64_t base, uint32_t limit) {
    // First 8 bytes (entry[index]) - lower part of 128-bit descriptor
    gdt[index].limit_low = limit & 0xFFFF;
    gdt[index].base_low = base & 0xFFFF;
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].access = 0x89;  // Present=1, DPL=0, Type=1001 (TSS available)
    gdt[index].granularity = ((limit >> 16) & 0x0F); // No G bit for TSS
    gdt[index].base_high = (base >> 24) & 0xFF;
    
    // Second 8 bytes (entry[index+1]) - upper part of 128-bit descriptor
    // Contains upper 32 bits of base address
    uint32_t base_upper = (base >> 32) & 0xFFFFFFFF;
    gdt[index + 1].limit_low = base_upper & 0xFFFF;
    gdt[index + 1].base_low = (base_upper >> 16) & 0xFFFF;
    gdt[index + 1].base_middle = 0;
    gdt[index + 1].access = 0;
    gdt[index + 1].granularity = 0;
    gdt[index + 1].base_high = 0;
}

void gdt_init(void) {
    terminal_writestring("[GDT] Initializing Global Descriptor Table...\n");
    
    gdt_set_entry(0, 0, 0, 0, 0);
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xA0);
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xA0);
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xA0);
    
    // TSS entry (128-bit descriptor uses entries 5 and 6)
    uint64_t tss_addr = tss_get();
    gdt_set_tss_entry(5, tss_addr, sizeof(tss_t) - 1);
    
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;
    
    __asm__ volatile("lgdt %0" : : "m"(gdt_ptr));
    
    terminal_writestring("[GDT] Loaded successfully\n");
    terminal_writestring("  [0] NULL\n");
    terminal_writestring("  [1] Kernel Code (ring 0) at 0x08\n");
    terminal_writestring("  [2] Kernel Data (ring 0) at 0x10\n");
    terminal_writestring("  [3] User Code (ring 3) at 0x18\n");
    terminal_writestring("  [4] User Data (ring 3) at 0x20\n");
    terminal_writestring("  [5,6] TSS (128-bit) at 0x28\n");
    
    gdt_reload_segments();
}

void gdt_load_tss(void) {
    // TSS is at selector 0x28, entries 5 and 6 (128-bit descriptor)
    uint64_t tss_addr = tss_get();
    
    // Update base address in both parts of the 128-bit descriptor
    // Entry 5: lower 24 bits of base
    gdt[5].base_low = tss_addr & 0xFFFF;
    gdt[5].base_middle = (tss_addr >> 16) & 0xFF;
    gdt[5].base_high = (tss_addr >> 24) & 0xFF;
    
    // Entry 6: upper 32 bits of base
    uint32_t base_upper = (tss_addr >> 32) & 0xFFFFFFFF;
    gdt[6].limit_low = base_upper & 0xFFFF;
    gdt[6].base_low = (base_upper >> 16) & 0xFFFF;
    
    // Load TSS selector into TR
    __asm__ volatile("ltr %%ax" : : "a"(GDT_TSS));
    
    terminal_writestring("[GDT] TSS loaded at 0x");
    char buf[32];
    uint64_to_hex(tss_addr, buf);
    terminal_writestring(buf);
    terminal_writestring(" (128-bit descriptor)\n");
}

void gdt_reload_segments(void) {
    __asm__ volatile(
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "rax", "memory"
    );
    
    terminal_writestring("[GDT] Segment registers reloaded\n");
}
