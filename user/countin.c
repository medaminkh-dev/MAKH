/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Read stdin to EOF and exit with the byte count — a pipeline consumer. */
#include "usys.h"

int umain(void) {
    char buf[64];
    int total = 0;
    for (;;) {
        long r = uread(0, buf, sizeof(buf));
        if (r <= 0) break;
        total += (int)r;
    }
    return total;
}
