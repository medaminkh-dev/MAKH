/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* whoami: print the effective user name. MAKH is single-user, so uid 0 = root. */
#include "usys.h"

int umain(void) {
    const char* u = (ugetuid() == 0) ? "root\n" : "user\n";
    unsigned long n = 0;
    while (u[n]) n++;
    uwrite(1, u, n);
    return 0;
}
