/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise cwd + relative paths end to end; return 0 iff every step holds. */
#include "usys.h"

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int umain(void) {
    char buf[64];

    if (ugetcwd(buf, sizeof(buf)) <= 0) return 1;
    if (!streq(buf, "/")) return 2;              /* fresh process starts at / */

    if (uchdir("/bin") != 0) return 3;
    if (ugetcwd(buf, sizeof(buf)) <= 0) return 4;
    if (!streq(buf, "/bin")) return 5;

    /* a relative open resolves against the cwd: /bin/hello exists in the initrd */
    long fd = uopen("hello", 0 /*O_RDONLY*/);
    if (fd < 0) return 6;
    uclose((int)fd);

    /* ".." climbs back to the root */
    if (uchdir("..") != 0) return 7;
    if (ugetcwd(buf, sizeof(buf)) <= 0) return 8;
    if (!streq(buf, "/")) return 9;

    /* a messy path is canonicalised */
    if (uchdir("/bin/./../bin") != 0) return 10;
    if (ugetcwd(buf, sizeof(buf)) <= 0) return 11;
    if (!streq(buf, "/bin")) return 12;

    return 0;
}
