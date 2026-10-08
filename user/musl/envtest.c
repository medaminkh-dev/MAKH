/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * MakhOS Phase 20-S (G3): prove execve() carries envp. Started with no
 * environment, this re-execs itself with MAKH_G3=42 in envp; the second
 * instance reads it back via getenv() (which musl initialises from the envp
 * laid on the SysV stack) and exits with its value. Exit 42 == envp survived.
 */
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    char* v = getenv("MAKH_G3");
    if (v) return atoi(v);                       /* child: exit with the value */
    char* nargv[] = { "/bin/envtest", 0 };
    char* nenvp[] = { "MAKH_G3=42", 0 };
    execve("/bin/envtest", nargv, nenvp);
    return 7;                                    /* only if execve failed */
}
