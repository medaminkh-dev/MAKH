/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* A foreground job that never returns on its own — only a signal (^C) ends it.
 * Used by the job-control test to prove Ctrl+C reaches the running job. */
#include "usys.h"

int umain(void) {
    volatile unsigned long i = 0;
    for (;;) { i++; }           /* preemptible ring-3 spin; killed by SIGINT */
    return 0;                   /* unreachable */
}
