/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * ls: list directory contents (one name per line, dotfiles hidden). A directory
 * argument is listed; a regular-file argument prints its own name. With no
 * argument the current directory is listed. Entries are printed in the order the
 * filesystem returns them (no sort yet).
 */
#include "usys.h"

static unsigned long slen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void puts2(int fd, const char* s) { uwrite(fd, s, slen(s)); }

/* List one directory's entries. Returns 0 on success. */
static int list_dir(const char* path) {
    int fd = (int)uopen(path, 0);
    if (fd < 0) {
        puts2(2, "ls: ");
        puts2(2, path);
        puts2(2, ": cannot open\n");
        return 1;
    }
    unsigned long buf[256];                      /* 2 KiB, 8-byte aligned */
    for (;;) {
        long n = ugetdents64(fd, buf, sizeof(buf));
        if (n < 0) { uclose(fd); return 1; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct linux_dirent64* d =
                (struct linux_dirent64*)((char*)buf + off);
            if (d->d_name[0] != '.') {           /* hide dotfiles (and . ..) */
                puts2(1, d->d_name);
                uwrite(1, "\n", 1);
            }
            off += d->d_reclen;
        }
    }
    uclose(fd);
    return 0;
}

int umain(int argc, char** argv) {
    if (argc < 2)
        return list_dir(".");

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (ustat(argv[i], &st) < 0) {
            puts2(2, "ls: ");
            puts2(2, argv[i]);
            puts2(2, ": not found\n");
            rc = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (argc > 2) { puts2(1, argv[i]); puts2(1, ":\n"); }
            if (list_dir(argv[i]) != 0) rc = 1;
            if (argc > 2 && i + 1 < argc) uwrite(1, "\n", 1);
        } else {
            puts2(1, argv[i]);
            uwrite(1, "\n", 1);
        }
    }
    return rc;
}
