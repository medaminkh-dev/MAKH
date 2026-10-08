/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * MakhOS Phase 20-O: the first real-libc program.
 *
 * This is an ordinary C program built against musl libc as a static-PIE and
 * run on MAKH unchanged. It deliberately touches the paths a C runtime needs
 * end to end, so a clean exit proves the whole libc ABI works:
 *   - crt startup + self-relocation (rcrt1, R_X86_64_RELATIVE);
 *   - thread-local storage via %fs (arch_prctl + PT_TLS)  -> the __thread var;
 *   - the heap (brk/mmap)                                 -> malloc/free;
 *   - buffered stdio (writev)                             -> printf;
 *   - argv/auxv on the SysV initial stack                 -> argc/argv[0];
 *   - the exit path (exit_group).
 *
 * It exits 42 iff every check held — the sentinel the in-kernel KTEST waits on.
 * Rebuild with `make musl-progs` (needs the pip `ziglang` toolchain); the
 * committed binary beside this file is what the normal build ships.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__thread int tls_canary = 0xABCD;      /* lands in PT_TLS, reached via %fs */

int main(int argc, char **argv) {
    char *heap = malloc(64);
    if (!heap) return 1;
    strcpy(heap, "musl");

    if (tls_canary != 0xABCD) return 2; /* TLS initialised wrong */
    tls_canary = 7;                     /* and it is writable      */
    if (tls_canary != 7) return 3;

    int n = printf("hello from %s libc on MAKH: argc=%d argv0=%s tls=%d\n",
                   heap, argc, argc > 0 ? argv[0] : "(none)", tls_canary);
    if (n <= 0) return 4;               /* printf saw an output error */
    if (fflush(stdout) != 0) return 5;  /* the writev(fd 1) flush failed */

    free(heap);
    return 42;
}
