/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_KLOG_H
#define MAKHOS_KLOG_H

#include <types.h>

/**
 * =============================================================================
 * klog.h - Kernel formatted logging
 * =============================================================================
 * A tiny printf-style formatter for the kernel. Output goes to the VGA
 * terminal and the serial port (through terminal_writestring), so it is
 * captured by QEMU's serial log.
 *
 * Supported conversions:
 *   %s  - const char*          %c  - char
 *   %d / %i - signed int       %u  - unsigned int
 *   %x / %X - unsigned hex      %p  - pointer (0x...)
 *   %%  - literal percent
 * Length modifiers 'l' and 'll' work with d/i/u/x/X (e.g. %lu, %llx).
 * A field width with optional zero padding is honoured for integers,
 * e.g. %08x or %5d.
 * =============================================================================
 */

/* Log levels - lower is more severe. */
typedef enum {
    KLOG_ERROR = 0,
    KLOG_WARN  = 1,
    KLOG_INFO  = 2,
    KLOG_DEBUG = 3,
} klog_level_t;

/* Runtime threshold: messages with a level <= threshold are printed. */
void klog_set_level(klog_level_t level);
klog_level_t klog_get_level(void);

/* Core formatting primitive (used by kprintf and friends). */
void kvprintf(const char* fmt, __builtin_va_list args);
void kprintf(const char* fmt, ...);

/* Level-tagged logging. klog() prints "[tag] ..." only if enabled. */
void klog(klog_level_t level, const char* tag, const char* fmt, ...);

#define KLOG_E(tag, ...) klog(KLOG_ERROR, tag, __VA_ARGS__)
#define KLOG_W(tag, ...) klog(KLOG_WARN,  tag, __VA_ARGS__)
#define KLOG_I(tag, ...) klog(KLOG_INFO,  tag, __VA_ARGS__)
#define KLOG_D(tag, ...) klog(KLOG_DEBUG, tag, __VA_ARGS__)

#endif /* MAKHOS_KLOG_H */
