/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* echo: write argv[1..] space-separated, and exit with argc (for the tests). */
#include "usys.h"

static unsigned long slen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

int umain(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        uwrite(1, argv[i], slen(argv[i]));
        uwrite(1, (i + 1 < argc) ? " " : "\n", 1);
    }
    return argc;
}
