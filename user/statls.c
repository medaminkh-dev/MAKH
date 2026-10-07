/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise stat/fstat/getdents64/fcntl and exit 88 iff all behave:
 *   - /bin/hello is a regular non-empty file, /bin is a directory,
 *   - stdout (fd 1) is a character device,
 *   - listing /bin finds "hello" among its entries,
 *   - fcntl(F_GETFL) succeeds on an open file. */
#include "usys.h"

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int umain(void) {
    struct stat st;

    if (ustat("/bin/hello", &st) < 0) return 1;
    if (!S_ISREG(st.st_mode)) return 2;
    if (st.st_size <= 0) return 3;

    if (ustat("/bin", &st) < 0) return 4;
    if (!S_ISDIR(st.st_mode)) return 5;

    if (ufstat(1, &st) < 0) return 6;
    if ((st.st_mode & S_IFMT) != S_IFCHR) return 7;        /* stdout is a tty */

    int fd = uopen("/bin", 0);
    if (fd < 0) return 8;
    unsigned long buf[128];                                 /* 1 KiB, 8-aligned */
    int found = 0, count = 0;
    for (;;) {
        long n = ugetdents64(fd, buf, sizeof(buf));
        if (n < 0) { uclose(fd); return 9; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct linux_dirent64* d = (struct linux_dirent64*)((char*)buf + off);
            count++;
            if (streq(d->d_name, "hello")) found = 1;
            off += d->d_reclen;
        }
    }
    uclose(fd);
    if (count < 1) return 10;
    if (!found) return 11;

    int f2 = uopen("/bin/hello", 0);
    if (f2 < 0) return 12;
    if (ufcntl(f2, F_GETFL, 0) < 0) { uclose(f2); return 13; }
    uclose(f2);

    return 88;
}
