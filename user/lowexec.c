/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* A standard non-PIE executable linked at the conventional low address
 * 0x400000 (PML4 slot 0), proving the lower canonical half is the process's own
 * after the higher-half kernel migration (F21 Path A). It prints a marker (a
 * write() syscall, so it exercises the kernel under this process's slot-0-free
 * CR3) and exits with a distinctive code the test checks. */
#include "usys.h"
int umain(void) {
    uwrite(1, "lowexec ok\n", 11);
    return 77;                      /* exit code the KTEST asserts */
}
