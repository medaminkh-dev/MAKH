/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_lib.c
 * Tests for the freestanding string library.
 */

#include <ktest.h>
#include <lib/string.h>

KTEST(string, memset_and_memcmp) {
    char buf[64];
    memset(buf, 0xAB, sizeof(buf));
    for (int i = 0; i < 64; i++) KEXPECT_EQ((uint8_t)buf[i], 0xAB);

    char zero[64];
    memset(zero, 0, sizeof(zero));
    KEXPECT_NE(memcmp(buf, zero, 64), 0);
    memset(buf, 0, sizeof(buf));
    KEXPECT_EQ(memcmp(buf, zero, 64), 0);
}

KTEST(string, memcpy_nonoverlapping) {
    char src[32], dst[32];
    for (int i = 0; i < 32; i++) src[i] = (char)(i + 1);
    memset(dst, 0, sizeof(dst));
    memcpy(dst, src, 32);
    KEXPECT_EQ(memcmp(src, dst, 32), 0);
}

KTEST(string, memmove_overlap_forward) {
    char b[16];
    for (int i = 0; i < 16; i++) b[i] = (char)i;
    /* Overlapping move up by 4: dst > src. */
    memmove(b + 4, b, 8);
    for (int i = 0; i < 8; i++) KEXPECT_EQ((uint8_t)b[4 + i], (uint8_t)i);
}

KTEST(string, strlen_strcmp_strcpy) {
    KEXPECT_EQ((int)strlen(""), 0);
    KEXPECT_EQ((int)strlen("makh"), 4);

    char dst[16];
    strcpy(dst, "hello");
    KEXPECT_EQ((int)strlen(dst), 5);
    KEXPECT_EQ(strcmp(dst, "hello"), 0);
    KEXPECT_NE(strcmp(dst, "hellp"), 0);
    KEXPECT_EQ(strncmp("abcXYZ", "abcQQQ", 3), 0);
    KEXPECT_NE(strncmp("abcXYZ", "abcQQQ", 4), 0);
}

KTEST(string, strchr) {
    const char* s = "a/b/c";
    KEXPECT_EQ(strchr(s, '/') - s, 1);
    KEXPECT_EQ(strrchr(s, '/') - s, 3);
    KEXPECT((void*)strchr(s, 'z') == (void*)0);
}
