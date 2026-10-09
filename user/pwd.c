/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* pwd: print the current working directory. */
#include "usys.h"

int umain(void) {
    char buf[256];
    long n = ugetcwd(buf, sizeof(buf) - 1);
    if (n < 0) {
        uwrite(2, "pwd: cannot get cwd\n", 20);
        return 1;
    }
    /* ugetcwd returns the length (incl. or excl. NUL depending on the ABI); be
     * robust and measure the string. */
    unsigned long len = 0;
    while (len < sizeof(buf) && buf[len]) len++;
    buf[len] = '\n';
    uwrite(1, buf, len + 1);
    return 0;
}
