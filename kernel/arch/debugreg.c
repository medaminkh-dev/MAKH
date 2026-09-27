/**
 * MakhOS - debugreg.c
 * x86 hardware watchpoints. See debugreg.h.
 *
 * DR7 layout per slot n: L(n) enable bit at 2n, R/W(n) at 16+4n, LEN(n) at
 * 18+4n. LEN encodings: 00=1, 01=2, 11=4, 10=8 bytes.
 */

#include <arch/debugreg.h>
#include <irq.h>

static uint64_t read_dr7(void) {
    uint64_t v;
    __asm__ volatile("mov %%dr7, %0" : "=r"(v));
    return v;
}

static void write_dr7(uint64_t v) {
    __asm__ volatile("mov %0, %%dr7" :: "r"(v));
}

static void write_addr(int slot, uint64_t a) {
    switch (slot) {
        case 0: __asm__ volatile("mov %0, %%dr0" :: "r"(a)); break;
        case 1: __asm__ volatile("mov %0, %%dr1" :: "r"(a)); break;
        case 2: __asm__ volatile("mov %0, %%dr2" :: "r"(a)); break;
        case 3: __asm__ volatile("mov %0, %%dr3" :: "r"(a)); break;
    }
}

int hw_watch_set(int slot, const void* addr, int len, int kind) {
    uint64_t lenbits;
    switch (len) {
        case 1: lenbits = 0; break;
        case 2: lenbits = 1; break;
        case 4: lenbits = 3; break;
        case 8: lenbits = 2; break;
        default: return -1;
    }
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (slot < 0 || slot > 3 || (a & (uint64_t)(len - 1))) return -1;
    if (kind != HWW_WRITE && kind != HWW_READWRITE) return -1;

    irqflags_t f = local_irq_save();
    write_addr(slot, a);
    uint64_t dr7 = read_dr7();
    dr7 &= ~((3ULL << (2 * slot)) | (0xFULL << (16 + 4 * slot)));
    dr7 |= (1ULL << (2 * slot));
    dr7 |= ((uint64_t)kind << (16 + 4 * slot)) | (lenbits << (18 + 4 * slot));
    write_dr7(dr7);
    local_irq_restore(f);
    return 0;
}

void hw_watch_clear(int slot) {
    if (slot < 0 || slot > 3) return;
    irqflags_t f = local_irq_save();
    write_dr7(read_dr7() & ~(3ULL << (2 * slot)));
    local_irq_restore(f);
}
