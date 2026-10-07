/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Exercise pipe + fork (fd inheritance) + dup2 redirection, exit 55 on success:
 *   - a child writes "hello" down a pipe; the parent reads exactly that and
 *     then sees EOF once the child closes its end and exits;
 *   - a second child dup2's the pipe onto stdout and `write(1,...)` reaches it. */
#include "usys.h"

int umain(void) {
    int fds[2];
    if (upipe(fds) < 0) return 1;

    long pid = ufork();
    if (pid < 0) return 2;
    if (pid == 0) {                       /* child: producer */
        uclose(fds[0]);
        uwrite(fds[1], "hello", 5);
        uclose(fds[1]);
        return 0;
    }
    uclose(fds[1]);                        /* parent: only the read end */
    char buf[16];
    int got = 0;
    for (;;) {
        long r = uread(fds[0], buf + got, sizeof(buf) - got);
        if (r <= 0) break;                 /* EOF: writers gone */
        got += (int)r;
    }
    uclose(fds[0]);
    int st = -1; uwaitpid((int)pid, &st);
    if (got != 5) return 3;
    if (buf[0]!='h'||buf[1]!='e'||buf[2]!='l'||buf[3]!='l'||buf[4]!='o') return 4;

    /* dup2: redirect a child's stdout into a pipe. */
    int p2[2];
    if (upipe(p2) < 0) return 5;
    long pid2 = ufork();
    if (pid2 < 0) return 6;
    if (pid2 == 0) {
        uclose(p2[0]);
        udup2(p2[1], 1);                   /* stdout now the pipe */
        uclose(p2[1]);
        uwrite(1, "Z", 1);                 /* lands in the pipe, not the console */
        return 0;
    }
    uclose(p2[1]);
    char c = 0;
    uread(p2[0], &c, 1);
    uclose(p2[0]);
    int st2 = -1; uwaitpid((int)pid2, &st2);
    if (c != 'Z') return 7;

    return 55;
}
