/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* id: print the real user and group ids. MAKH is single-user (uid 0 = root). */
#include "usys.h"

static int putu(char* b, unsigned long v) {
    char t[20];
    int n = 0;
    if (!v) { b[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) b[i] = t[n - 1 - i];
    return n;
}

static int puts_(char* b, const char* s) {
    int n = 0;
    while (s[n]) { b[n] = s[n]; n++; }
    return n;
}

int umain(void) {
    unsigned long uid = (unsigned long)ugetuid();
    unsigned long gid = (unsigned long)ugetgid();
    const char* un = (uid == 0) ? "root" : "user";
    const char* gn = (gid == 0) ? "root" : "user";
    char b[80];
    int p = 0;
    p += puts_(b + p, "uid=");
    p += putu(b + p, uid);
    b[p++] = '(';
    p += puts_(b + p, un);
    b[p++] = ')';
    p += puts_(b + p, " gid=");
    p += putu(b + p, gid);
    b[p++] = '(';
    p += puts_(b + p, gn);
    b[p++] = ')';
    b[p++] = '\n';
    uwrite(1, b, (unsigned long)p);
    return 0;
}
