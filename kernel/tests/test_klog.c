/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_klog.c
 * Tests the kprintf number formatter by rendering into a string buffer.
 *
 * kprintf writes to the console, so to check its output we temporarily redirect
 * through ksnprintf-like behaviour is not available; instead we validate the
 * lower-level integer/hex conversions used everywhere (uint64_to_string /
 * uint64_to_hex from kernel.c) which kprintf mirrors.
 */

#include <ktest.h>
#include <kernel.h>
#include <lib/string.h>

KTEST(fmt, uint64_to_string_basic) {
    char b[32];
    uint64_to_string(0, b);           KEXPECT_EQ(strcmp(b, "0"), 0);
    uint64_to_string(7, b);           KEXPECT_EQ(strcmp(b, "7"), 0);
    uint64_to_string(12345, b);       KEXPECT_EQ(strcmp(b, "12345"), 0);
    uint64_to_string(4294967296ULL, b); KEXPECT_EQ(strcmp(b, "4294967296"), 0);
}

KTEST(fmt, uint64_to_hex_basic) {
    char b[32];
    uint64_to_hex(0, b);              KEXPECT_EQ(strcmp(b, "0"), 0);
    uint64_to_hex(0xDEADBEEF, b);     KEXPECT_EQ(strcmp(b, "0xDEADBEEF"), 0);
    uint64_to_hex(0x1000, b);         KEXPECT_EQ(strcmp(b, "0x1000"), 0);
}
