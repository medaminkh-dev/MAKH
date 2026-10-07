/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - panic.c
 * Kernel panic + halt with a frame-pointer backtrace.
 *
 * The kernel is built with -fno-omit-frame-pointer, so each stack frame stores
 * the previous RBP at [rbp] and the return address at [rbp+8]. We walk that
 * chain to print a backtrace, which is invaluable when a fuzzing run trips an
 * assertion deep in the kernel.
 */

#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <cmdline.h>

/* Symbols from linker.ld bounding the kernel image, used to sanity-check
 * return addresses before printing them. */
extern char KERNEL_END[];
#define KERNEL_TEXT_BASE 0x100000ULL

static int looks_like_code(uint64_t addr) {
    return addr >= KERNEL_TEXT_BASE && addr < (uint64_t)(uintptr_t)KERNEL_END;
}

void kernel_backtrace_from(uint64_t rbp) {
    kprintf("Backtrace:\n");
    for (int depth = 0; depth < 24 && rbp; depth++) {
        uint64_t* frame = (uint64_t*)(uintptr_t)rbp;
        /* Guard against a wild RBP that would fault the panic handler. */
        if (rbp < 0x1000 || (rbp & 0x7)) break;
        uint64_t ret = frame[1];
        if (!looks_like_code(ret)) break;
        kprintf("  #%d  %p\n", depth, (void*)(uintptr_t)ret);
        uint64_t next = frame[0];
        if (next <= rbp) break;  /* frame pointers must ascend */
        rbp = next;
    }
}

void kernel_backtrace(void) {
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    kernel_backtrace_from(rbp);
}

void kernel_halt(void) {
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/* Simple string panic (kept for existing callers). */
void kernel_panic(const char* message) {
    __asm__ volatile("cli");
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    kprintf("\n\n*** KERNEL PANIC ***\n%s\n", message ? message : "(no message)");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    kernel_backtrace();

    /* If running under the test harness, fail loudly instead of hanging. */
    if (cmdline_has("makh.test")) {
        qemu_debug_exit(2);
    }
    kernel_halt();
}

/* Formatted panic. */
void panic(const char* fmt, ...) {
    __asm__ volatile("cli");
    terminal_set_quiet(0);                   /* a panic is never silenced */
    terminal_setcolor(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_RED));
    kprintf("\n\n*** KERNEL PANIC ***\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));

    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    kvprintf(fmt, args);
    __builtin_va_end(args);
    kprintf("\n");

    kernel_backtrace();

    if (cmdline_has("makh.test")) {
        qemu_debug_exit(2);
    }
    kernel_halt();
}
