/**
 * MakhOS - kernel.h
 * Main kernel header
 */

#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/* Kernel version */
#define KERNEL_NAME     "MakhOS"
#define KERNEL_VERSION  "0.0.2"
#define KERNEL_PHASE    "Phase 2 Complete"

/* Kernel main entry point - called from boot.asm */
void kernel_main(void);

/* Utility functions (from kernel.c) */
void uint64_to_string(uint64_t value, char* buf);
void uint64_to_hex(uint64_t value, char* buf);

/* System halt functions */
void kernel_halt(void);
void kernel_panic(const char* message);

/* Port I/O functions */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* CPU control */
static inline void cli(void) {
    __asm__ volatile("cli");
}

static inline void sti(void) {
    __asm__ volatile("sti");
}

static inline void hlt(void) {
    __asm__ volatile("hlt");
}

#endif /* KERNEL_H */
