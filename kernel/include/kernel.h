/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - kernel.h
 * Main kernel header
 */

#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

/* Kernel version */
#define KERNEL_NAME     "MakhOS"
#define KERNEL_VERSION  "1.0.0"
#define KERNEL_PHASE    "Phase 28 (interactive shell)"

/* Kernel main entry point - called from boot.asm */
void kernel_main(void);

/* Utility functions (from kernel.c) */
void uint64_to_string(uint64_t value, char* buf);
void uint64_to_hex(uint64_t value, char* buf);

/* System halt functions (kernel/panic.c) */
void kernel_halt(void) __attribute__((noreturn));
void kernel_panic(const char* message);
void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void kernel_backtrace(void);
void kernel_backtrace_from(uint64_t rbp);   /* walk frames starting at rbp */

/* Port I/O functions */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/*
 * qemu_debug_exit - power off QEMU with an exit code.
 * Requires "-device isa-debug-exit,iobase=0xf4,iosize=0x04". QEMU exits with
 * process status ((code << 1) | 1), so code 0 => status 1 (see tools/run_tests.py).
 */
static inline void qemu_debug_exit(uint8_t code) {
    outl(0xf4, code);
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
