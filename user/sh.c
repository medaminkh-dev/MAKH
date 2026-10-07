/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * A tiny shell: print a prompt, read a line, run it as a program (fork+execve),
 * wait, and repeat. A line is treated as a command path (absolute, or relative
 * to the cwd). On EOF (^D) the shell exits with the last command's status.
 * Arguments are not parsed yet — that arrives with argv in a later brick.
 */
#include "usys.h"

int umain(void) {
    char line[128];
    int last = 0;

    for (;;) {
        uwrite(1, "$ ", 2);
        long n = uread(0, line, sizeof(line) - 1);
        if (n <= 0) return last;                 /* EOF: leave with last status */
        if (line[n - 1] == '\n') n--;            /* drop the newline */
        line[n] = '\0';
        if (n == 0) continue;                    /* blank line */

        long pid = ufork();
        if (pid < 0) { last = 127; continue; }
        if (pid == 0) {                          /* child: become the command */
            uexecve(line, 0, 0);
            return 127;                          /* exec failed (no such command) */
        }
        int st = -1;                             /* parent: wait for it */
        uwaitpid((int)pid, &st);
        last = st;
    }
}
