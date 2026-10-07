/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * A tiny shell with job control and pipelines: prompt, read a line, split it on
 * '|' into stages, and run them connected by pipes (fork + dup2 + execve). A
 * single command keeps full job control (its own process group owns the
 * terminal, so Ctrl+C hits it, not the shell). On EOF (^D) the shell exits with
 * the last command's status.
 */
#include "usys.h"

#define MAXARGS   16
#define MAXSTAGES 8

static int tokenize(char* s, char** argv) {
    int ac = 0, i = 0;
    for (;;) {
        while (s[i] == ' ') i++;
        if (!s[i] || ac >= MAXARGS - 1) break;
        argv[ac++] = &s[i];
        while (s[i] && s[i] != ' ') i++;
        if (s[i]) s[i++] = '\0';
    }
    argv[ac] = 0;
    return ac;
}

/* Split `line` at each '|' into stage strings (in place). Returns the count. */
static int split_pipes(char* line, char** stages) {
    int ns = 0, start = 0;
    for (int i = 0;; i++) {
        if (line[i] == '|' || line[i] == '\0') {
            char end = line[i];
            line[i] = '\0';
            if (ns < MAXSTAGES) stages[ns++] = &line[start];
            start = i + 1;
            if (!end) break;
        }
    }
    return ns;
}

/* One command, no pipe, with job control. Returns its exit status. */
static int run_single(char* cmd, int shell_pgid) {
    char* argv[MAXARGS];
    if (tokenize(cmd, argv) == 0) return 0;
    long pid = ufork();
    if (pid < 0) return 127;
    if (pid == 0) {                              /* child: own group, exec    */
        usetpgid(0, 0);
        uexecve(argv[0], argv, 0);
        return 127;
    }
    usetpgid((int)pid, (int)pid);                /* race-safe group assignment */
    utcsetpgrp(0, (int)pid);                     /* hand the job the terminal  */
    int st = -1;
    uwaitpid((int)pid, &st);
    utcsetpgrp(0, shell_pgid);                   /* shell reclaims it          */
    return st;
}

/* An N-stage pipeline, stages connected by pipes. Returns the last status. */
static int run_pipeline(char** stages, int ns) {
    int in_fd = 0;
    int pids[MAXSTAGES];
    for (int i = 0; i < ns; i++) {
        int pfd[2], out_fd = 1;
        if (i < ns - 1) { if (upipe(pfd) < 0) return 127; out_fd = pfd[1]; }
        long pid = ufork();
        if (pid < 0) return 127;
        if (pid == 0) {                          /* stage i */
            if (in_fd != 0)  { udup2(in_fd, 0);  uclose(in_fd); }
            if (out_fd != 1) { udup2(out_fd, 1); uclose(out_fd); }
            if (i < ns - 1)  uclose(pfd[0]);     /* not this stage's read end */
            char* argv[MAXARGS];
            if (tokenize(stages[i], argv) && argv[0]) uexecve(argv[0], argv, 0);
            return 127;
        }
        pids[i] = (int)pid;
        if (in_fd != 0) uclose(in_fd);           /* parent is done with it     */
        if (i < ns - 1) { uclose(pfd[1]); in_fd = pfd[0]; }
    }
    int st = 0;
    for (int i = 0; i < ns; i++) { int s = -1; uwaitpid(pids[i], &s); st = s; }
    return st;                                   /* status of the last stage   */
}

int umain(void) {
    char line[128];
    int  last = 0;
    int  shell_pgid = (int)ugetpgrp();

    for (;;) {
        uwrite(1, "$ ", 2);
        long n = uread(0, line, sizeof(line) - 1);
        if (n <= 0) return last;                 /* EOF: leave with last status */
        if (line[n - 1] == '\n') n--;
        line[n] = '\0';

        int blank = 1;
        for (int k = 0; line[k]; k++) if (line[k] != ' ') { blank = 0; break; }
        if (blank) continue;

        int piped = 0;
        for (int k = 0; line[k]; k++) if (line[k] == '|') { piped = 1; break; }
        if (piped) {
            char* stages[MAXSTAGES];
            int ns = split_pipes(line, stages);
            if (ns > 0) last = run_pipeline(stages, ns);
        } else {
            last = run_single(line, shell_pgid);
        }
    }
}
