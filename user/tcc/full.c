/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* A real libc C program — <stdio.h>, <stdlib.h>, <string.h>, printf/malloc —
 * that tcc compiles and links against musl WHILE RUNNING ON MAKH (F21-b). Links
 * static + non-PIE, so it lands at 0x400000 and the higher-half kernel loads
 * it; musl's crt + __libc_start_main bring it up on MAKH's syscall surface.
 * Exits 42, which the KTEST checks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char* s = malloc(32);
    if (!s) return 1;
    strcpy(s, "hello");
    printf("%s from a full libc program on MAKH, %d\n", s, 20 + 22);
    int rc = (strlen(s) == 5) ? 42 : 1;
    free(s);
    return rc;
}
