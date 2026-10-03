/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_CMDLINE_H
#define MAKHOS_CMDLINE_H

#include <types.h>

/**
 * =============================================================================
 * cmdline.h - Kernel command line access
 * =============================================================================
 * The bootloader (GRUB) passes a command line string via the Multiboot2
 * information structure. We use it to select boot behaviour, most importantly
 * "makh.test" which makes the kernel run its self-tests and then power off
 * with a pass/fail exit code (used by `make test` and CI).
 * =============================================================================
 */

/* Parse the Multiboot2 cmdline tag. Safe to call with a bad/zero address. */
void cmdline_init(uint64_t mb_info_addr);

/* The raw command line string (never NULL; "" if none). */
const char* cmdline_get(void);

/* Returns non-zero if the whitespace-separated token `flag` is present. */
int cmdline_has(const char* flag);

#endif /* MAKHOS_CMDLINE_H */
