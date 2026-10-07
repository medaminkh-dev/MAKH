/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* execve /bin/echo with a fixed argv; becomes echo, which exits with argc (4). */
#include "usys.h"

int umain(void) {
    char* argv[] = { "/bin/echo", "x", "y", "z", 0 };
    uexecve("/bin/echo", argv, 0);
    return 127;                         /* only reached if execve failed */
}
