/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * A tiny shell with job control: print a prompt, read a line, split it into an
 * argv, then run it as a foreground job (fork + execve + wait). Each command
 * runs in its own process group, and the shell hands the terminal to that group
 * while it runs — so Ctrl+C reaches the job, not the shell. On EOF (^D) the
 * shell exits with the last command's status.
 */
#include "usys.h"

int umain(void) {
    char  line[128];
    char* argv[16];
    int   last = 0;
    int   shell_pgid = (int)ugetpgrp();          /* the shell leads its own group */

    for (;;) {
        uwrite(1, "$ ", 2);
        long n = uread(0, line, sizeof(line) - 1);
        if (n <= 0) return last;                 /* EOF: leave with last status */
        if (line[n - 1] == '\n') n--;            /* drop the newline */
        line[n] = '\0';

        /* Split the line into whitespace-separated argv (argv[0] = command). */
        int ac = 0, i = 0;
        while (i < n && ac < 15) {
            while (i < n && line[i] == ' ') i++;
            if (i >= n) break;
            argv[ac++] = &line[i];
            while (i < n && line[i] != ' ') i++;
            if (i < n) line[i++] = '\0';
        }
        argv[ac] = 0;
        if (ac == 0) continue;                   /* blank line */

        long pid = ufork();
        if (pid < 0) { last = 127; continue; }
        if (pid == 0) {                          /* child: own group, become cmd */
            usetpgid(0, 0);                      /* lead a new group (race-safe)  */
            uexecve(argv[0], argv, 0);
            return 127;                          /* exec failed (no such command) */
        }
        /* Parent: put the job in its own group and give it the terminal. Both
         * sides call setpgid so the group is set no matter who runs first. */
        usetpgid((int)pid, (int)pid);
        utcsetpgrp(0, (int)pid);                 /* the job is the foreground now  */

        int st = -1;
        uwaitpid((int)pid, &st);                 /* wait for the job to finish     */
        utcsetpgrp(0, shell_pgid);               /* shell reclaims the terminal    */
        last = st;
    }
}
