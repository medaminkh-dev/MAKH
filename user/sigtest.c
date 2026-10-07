/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Install a SIGTERM handler, raise it, and let it run: the handler sets a flag,
 * sigreturn restores us here, and we exit 42 — proving install + deliver +
 * handler-ran + sigreturn-resumed all work. */
#include "usys.h"

static volatile int got = 0;
static void onsig(int s) { (void)s; got = 42; }

int umain(void) {
    usignal(SIGTERM, onsig);
    uraise(SIGTERM);        /* delivered at this syscall's return, into onsig */
    return got;             /* 42 iff the handler ran and we resumed here */
}
