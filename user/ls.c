/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * ls: list directory contents.
 *   -a  also show entries starting with '.'
 *   -l  long format: type+permissions, link count, owner, size, name
 * With no path the current directory is listed; a file argument lists itself.
 * Entries are shown in the order the filesystem returns them (no sort yet).
 */
#include "usys.h"

static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts2(int fd, const char* s) { uwrite(fd, s, slen(s)); }

static int putu(char* b, unsigned long v) {
    char t[20]; int n = 0;
    if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}

static int opt_a = 0, opt_l = 0;

/* Join dir and name into out (handles "." and trailing '/'). */
static void join(char* out, const char* dir, const char* name) {
    int p = 0;
    if (!(dir[0] == '.' && dir[1] == 0)) {        /* "." -> use name as-is (cwd) */
        while (*dir) out[p++] = *dir++;
        if (p && out[p - 1] != '/') out[p++] = '/';
    }
    while (*name) out[p++] = *name++;
    out[p] = 0;
}

/* Print one long-format line for `name` whose metadata is `st`. */
static void long_line(const struct stat* st, const char* name) {
    char b[96];
    int p = 0;
    unsigned int m = st->st_mode;
    char tp = '-';
    if (S_ISDIR(m)) tp = 'd';
    else if (S_ISLNK(m)) tp = 'l';
    else if (S_ISCHR(m)) tp = 'c';
    b[p++] = tp;
    static const char* rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        b[p++] = (m & (0400 >> i)) ? rwx[i] : '-';
    b[p++] = ' ';
    p += putu(b + p, st->st_nlink ? (unsigned long)st->st_nlink : 1);
    { const char* o = " root root "; for (const char* s = o; *s; s++) b[p++] = *s; }
    p += putu(b + p, (unsigned long)(st->st_size < 0 ? 0 : st->st_size));
    b[p++] = ' ';
    uwrite(1, b, (unsigned long)p);
    puts2(1, name);
    uwrite(1, "\n", 1);
}

static int list_dir(const char* path) {
    int fd = (int)uopen(path, 0);
    if (fd < 0) { puts2(2, "ls: "); puts2(2, path); puts2(2, ": cannot open\n"); return 1; }
    unsigned long buf[256];                        /* 2 KiB, 8-byte aligned */
    char full[256];
    for (;;) {
        long n = ugetdents64(fd, buf, sizeof(buf));
        if (n < 0) { uclose(fd); return 1; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct linux_dirent64* d = (struct linux_dirent64*)((char*)buf + off);
            const char* nm = d->d_name;
            off += d->d_reclen;
            if (!opt_a && nm[0] == '.') continue;
            if (opt_l) {
                struct stat st;
                join(full, path, nm);
                if (ulstat(full, &st) < 0) { puts2(1, nm); puts2(1, "\n"); continue; }
                long_line(&st, nm);
            } else {
                puts2(1, nm);
                uwrite(1, "\n", 1);
            }
        }
    }
    uclose(fd);
    return 0;
}

int umain(int argc, char** argv) {
    const char* paths[16];
    int npath = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (char* f = argv[i] + 1; *f; f++) {
                if (*f == 'a') opt_a = 1;
                else if (*f == 'l') opt_l = 1;
            }
        } else if (npath < 16) {
            paths[npath++] = argv[i];
        }
    }
    if (npath == 0) return list_dir(".");

    int rc = 0;
    for (int i = 0; i < npath; i++) {
        struct stat st;
        if (ustat(paths[i], &st) < 0) {
            puts2(2, "ls: "); puts2(2, paths[i]); puts2(2, ": not found\n");
            rc = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (npath > 1) { puts2(1, paths[i]); puts2(1, ":\n"); }
            if (list_dir(paths[i]) != 0) rc = 1;
            if (npath > 1 && i + 1 < npath) uwrite(1, "\n", 1);
        } else if (opt_l) {
            long_line(&st, paths[i]);
        } else {
            puts2(1, paths[i]);
            uwrite(1, "\n", 1);
        }
    }
    return rc;
}
