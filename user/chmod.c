/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* chmod MODE FILE...: set a file's permission bits. MAKH's own implementation;
 * the mode is octal (e.g. 755, 0644, 600) — the three/four rwx digits. */
#include "usys.h"

static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void err(const char* s) { uwrite(2, s, slen(s)); }

int umain(int argc, char** argv) {
    if (argc < 3) { err("usage: chmod MODE FILE...\n"); return 2; }

    int mode = 0, ndig = 0;
    for (const char* p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '7') { err("chmod: invalid mode (octal only, e.g. 755)\n"); return 2; }
        mode = mode * 8 + (*p - '0'); ndig++;
    }
    if (ndig == 0 || ndig > 4) { err("chmod: invalid mode (octal only, e.g. 755)\n"); return 2; }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (uchmod(argv[i], mode) < 0) {
            err("chmod: "); err(argv[i]); err(": cannot change mode\n");
            rc = 1;
        }
    }
    return rc;
}
