/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include "usys.h"
int umain(void) {
    volatile unsigned long i;       /* CPU-bound: must be preemptible in ring 3 */
    for (i = 0; i < 30000000UL; i++) { }
    return 9;
}
