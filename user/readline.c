/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Read one line from stdin and exit with its byte count (0 = EOF). */
#include "usys.h"

int umain(void) {
    char buf[64];
    long n = uread(0, buf, sizeof(buf));
    if (n < 0) return 100;              /* error (e.g. -EINTR) */
    return (int)(n & 0x7f);            /* "hi\n" -> 3, EOF -> 0 */
}
