/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercises uname(2) and getrlimit(2) directly (U2 syscalls): exits 42 iff
 * uname reports sysname "MAKH" and an x86-family machine, and
 * getrlimit(RLIMIT_NOFILE) returns a nonzero current limit. Any other exit code
 * marks which check failed. */
#include "usys.h"
#define SYS_UNAME      63
#define SYS_GETRLIMIT  97
#define RLIMIT_NOFILE  7

int umain(void) {
    char uts[6 * 65];                       /* six NUL-padded 65-byte fields */
    if (usyscall(SYS_UNAME, (long)uts, 0, 0) != 0) return 1;

    const char* sysname = uts;              /* field 0 */
    if (!(sysname[0] == 'M' && sysname[1] == 'A' &&
          sysname[2] == 'K' && sysname[3] == 'H' && sysname[4] == '\0'))
        return 2;
    const char* machine = uts + 4 * 65;     /* field 4 */
    if (!(machine[0] == 'x' && machine[1] == '8' && machine[2] == '6'))
        return 3;

    unsigned long rl[2] = { 0, 0 };         /* { rlim_cur, rlim_max } */
    if (usyscall(SYS_GETRLIMIT, RLIMIT_NOFILE, (long)rl, 0) != 0) return 4;
    if (rl[0] == 0) return 5;               /* a real file-descriptor limit */

    return 42;
}
