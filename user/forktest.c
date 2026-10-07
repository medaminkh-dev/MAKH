/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* fork(): child exits 7; parent waits for it. Returns 0 iff both halves agree. */
#include "usys.h"

int umain(void) {
    long pid = ufork();
    if (pid < 0) return 1;              /* fork failed */
    if (pid == 0) return 7;             /* child: exit(7) */

    int st = -1;
    long w = uwaitpid((int)pid, &st);
    if (w != pid) return 2;             /* waited for the wrong child */
    if (st != 7) return 3;              /* child's exit code must arrive */
    return 0;
}
