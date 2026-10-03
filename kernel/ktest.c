/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - ktest.c
 * In-kernel test runner: discovers tests from the .ktests section, runs them,
 * and reports results.
 */

#include <ktest.h>
#include <klog.h>
#include <vga.h>
#include <lib/string.h>

/* Provided by the linker script (linker.ld) around the .ktests section.
 * The section holds pointers to descriptors (see the KTEST macro). */
extern ktest_ptr_t __ktests_start[];
extern ktest_ptr_t __ktests_end[];

/* Per-test bookkeeping (single-threaded runner, so globals are fine). */
static int current_failures = 0;
static int current_checks = 0;

void ktest_record_check(void) {
    current_checks++;
}

void ktest_record_fail(const char* file, int line, const char* expr) {
    current_failures++;
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
    kprintf("    FAIL %s:%d: %s\n", file, line, expr);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
}

int ktest_current_failures(void) {
    return current_failures;
}

static int run_filtered(const char* suite) {
    unsigned total = (unsigned)(__ktests_end - __ktests_start);
    unsigned run = 0, passed = 0, failed = 0;
    unsigned total_checks = 0;

    kprintf("\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    kprintf("======== MAKH kernel test suite ========\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    kprintf("registered tests: %u\n\n", total);

    for (ktest_ptr_t* pp = __ktests_start; pp < __ktests_end; pp++) {
        const ktest_t* t = *pp;
        if (suite && strcmp(suite, t->suite) != 0) continue;

        current_failures = 0;
        current_checks = 0;
        run++;

        kprintf("[ RUN  ] %s.%s\n", t->suite, t->name);
        t->fn();
        total_checks += (unsigned)current_checks;

        if (current_failures == 0) {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
            kprintf("[ PASS ] %s.%s (%d checks)\n", t->suite, t->name, current_checks);
            passed++;
        } else {
            terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
            kprintf("[ FAIL ] %s.%s (%d failures)\n", t->suite, t->name, current_failures);
            failed++;
        }
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
    }

    kprintf("\n");
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    kprintf("======== results: %u run, %u passed, %u failed, %u checks ========\n",
            run, passed, failed, total_checks);
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));

    return (int)failed;
}

int ktest_run_all(void) {
    return run_filtered(NULL);
}

int ktest_run_suite(const char* suite) {
    return run_filtered(suite);
}
