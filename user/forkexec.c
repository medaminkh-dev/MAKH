/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* The shell pattern: fork, child execve()s /bin/hello (exits 42), parent waits. */
#include "usys.h"

int umain(void) {
    long pid = ufork();
    if (pid < 0) return 1;
    if (pid == 0) {
        uexecve("/bin/hello", 0, 0);   /* becomes hello; never returns on success */
        return 99;                     /* exec failed */
    }
    int st = -1;
    if (uwaitpid((int)pid, &st) != pid) return 2;
    if (st != 42) return 3;            /* hello returns 42 */
    return 0;
}
