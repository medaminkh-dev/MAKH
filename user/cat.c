/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* cat: copy each file argument to stdout; with no arguments, copy stdin. */
#include "usys.h"

static unsigned long slen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int cat_fd(int fd) {
    char buf[1024];
    for (;;) {
        long n = uread(fd, buf, sizeof(buf));
        if (n < 0) return 1;
        if (n == 0) return 0;
        long off = 0;
        while (off < n) {
            long w = uwrite(1, buf + off, (unsigned long)(n - off));
            if (w <= 0) return 1;
            off += w;
        }
    }
}

int umain(int argc, char** argv) {
    if (argc < 2)
        return cat_fd(0);

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = (int)uopen(argv[i], 0);
        if (fd < 0) {
            uwrite(2, "cat: ", 5);
            uwrite(2, argv[i], slen(argv[i]));
            uwrite(2, ": cannot open\n", 14);
            rc = 1;
            continue;
        }
        if (cat_fd(fd) != 0) rc = 1;
        uclose(fd);
    }
    return rc;
}
