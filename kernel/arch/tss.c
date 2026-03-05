#include <arch/tss.h>
#include <lib/string.h>
#include <vga.h>
#include <kernel.h>

static tss_t tss;

void tss_init(void) {
    terminal_writestring("[TSS] Initializing Task State Segment...\n");
    memset(&tss, 0, sizeof(tss_t));
    tss.iomap_base = sizeof(tss_t);
    terminal_writestring("[TSS] Initialized at 0x");
    char buf[32];
    uint64_to_hex((uint64_t)&tss, buf);
    terminal_writestring(buf);
    terminal_writestring(", size ");
    uint64_to_string(sizeof(tss_t), buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
}

void tss_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
    terminal_writestring("[TSS] Kernel stack set to 0x");
    char buf[32];
    uint64_to_hex(rsp0, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

void tss_set_ist(uint8_t ist_index, uint64_t stack_top) {
    if (ist_index < 1 || ist_index > 7) return;
    uint64_t* ist = &tss.ist1 + (ist_index - 1);
    *ist = stack_top;
    terminal_writestring("[TSS] IST");
    char buf[32];
    uint64_to_string(ist_index, buf);
    terminal_writestring(buf);
    terminal_writestring(" set to 0x");
    uint64_to_hex(stack_top, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

uint64_t tss_get(void) {
    return (uint64_t)&tss;
}
