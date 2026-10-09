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

/* A small default environment handed to every child, so programs (and `env`)
 * see sensible values. MAKH has no `export` yet; this is fixed. */
static char* const sh_env[] = {
    "PATH=/bin:/usr/bin",
    "HOME=/",
    "USER=root",
    "TERM=makh",
    "SHELL=/bin/sh",
    0,
};

static int sh_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int sh_atoi(const char* s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}
static unsigned long sh_strlen(const char* s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * Exec argv[0] in the (already forked) child. A name containing '/' is used as
 * given; a bare name is looked up in /bin then /usr/bin, so `ls`, `uname`, ...
 * work without typing the full path. This never returns: on success the image is
 * replaced; if nothing is found it prints "sh: <cmd>: not found" and exits 127
 * (returning here would turn the child back into a second shell).
 */
static void sh_exec(char** argv) {
    if (!argv[0]) usyscall(SYS_EXIT, 127, 0, 0);

    int has_slash = 0;
    for (char* p = argv[0]; *p; p++) if (*p == '/') { has_slash = 1; break; }

    if (has_slash) {
        uexecve(argv[0], argv, sh_env);               /* returns only on failure */
    } else {
        static const char* const dirs[] = { "/bin/", "/usr/bin/" };
        char path[128];
        for (int d = 0; d < 2; d++) {
            int i = 0;
            for (const char* s = dirs[d]; *s && i < 120; s++) path[i++] = *s;
            for (char* s = argv[0]; *s && i < 127; s++)       path[i++] = *s;
            path[i] = '\0';
            uexecve(path, argv, sh_env);              /* returns only on failure */
        }
    }

    uwrite(2, "sh: ", 4);
    uwrite(2, argv[0], sh_strlen(argv[0]));
    uwrite(2, ": not found\n", 12);
    usyscall(SYS_EXIT, 127, 0, 0);
}

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

    /* Builtins must run in the shell process itself. */
    if (sh_streq(argv[0], "cd")) {
        const char* dir = argv[1] ? argv[1] : "/";
        if (uchdir(dir) < 0) {
            uwrite(2, "cd: ", 4);
            uwrite(2, dir, sh_strlen(dir));
            uwrite(2, ": no such directory\n", 20);
            return 1;
        }
        return 0;
    }
    if (sh_streq(argv[0], "exit")) {
        usyscall(SYS_EXIT_GROUP, argv[1] ? sh_atoi(argv[1]) : 0, 0, 0);
    }

    long pid = ufork();
    if (pid < 0) return 127;
    if (pid == 0) {                              /* child: own group, exec    */
        usetpgid(0, 0);
        sh_exec(argv);                           /* never returns             */
        usyscall(SYS_EXIT, 127, 0, 0);           /* unreachable               */
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
            if (tokenize(stages[i], argv) && argv[0]) sh_exec(argv);  /* no return */
            usyscall(SYS_EXIT, 127, 0, 0);
        }
        pids[i] = (int)pid;
        if (in_fd != 0) uclose(in_fd);           /* parent is done with it     */
        if (i < ns - 1) { uclose(pfd[1]); in_fd = pfd[0]; }
    }
    int st = 0;
    for (int i = 0; i < ns; i++) { int s = -1; uwaitpid(pids[i], &s); st = s; }
    return st;                                   /* status of the last stage   */
}

/* ------------------------------------------------------------------ line editor
 * An interactive editor with history, used when stdin is a terminal. It puts the
 * tty in raw mode (so it sees arrows, Ctrl-keys and Delete), draws the line
 * itself, and restores the tty before returning so children run with a normal
 * terminal. On a non-tty (a pipe or script) it falls back to a plain read.
 *   Up/Down = history, Left/Right/Home(^A)/End(^E) = move, ^U kill line,
 *   ^K kill to end, Backspace/Delete, ^D = EOF on an empty line.
 */
#define LINE_MAX  256
#define HIST_MAX  32
static char sh_hist[HIST_MAX][LINE_MAX];
static int  sh_hist_n;

static void hist_push(const char* s) {
    if (!s[0]) return;
    if (sh_hist_n > 0) {                          /* skip consecutive duplicate */
        const char* last = sh_hist[(sh_hist_n - 1) % HIST_MAX];
        if (sh_streq(last, s)) return;
    }
    char* d = sh_hist[sh_hist_n % HIST_MAX];
    int i = 0; for (; s[i] && i < LINE_MAX - 1; i++) d[i] = s[i]; d[i] = 0;
    sh_hist_n++;
}

static void refresh(const char* pr, int pl, const char* buf, int len, int cur) {
    uwrite(1, "\r", 1); uwrite(1, pr, (unsigned long)pl);
    uwrite(1, buf, (unsigned long)len); uwrite(1, "\033[K", 3);
    uwrite(1, "\r", 1); uwrite(1, pr, (unsigned long)pl);
    uwrite(1, buf, (unsigned long)cur);
}

/* Returns line length (>=0), or -1 on EOF. */
static int sh_readline(const char* pr, char* buf, int max) {
    int pl = 0; while (pr[pl]) pl++;
    struct termios saved;
    int israw = (ugettermios(0, &saved) >= 0);
    if (israw) { struct termios r = saved; r.c_lflag &= ~(unsigned)(ICANON | ECHO); usettermios(0, &r); }
    uwrite(1, pr, (unsigned long)pl);
    if (!israw) {                                  /* pipe/script: plain read */
        long n = uread(0, buf, (unsigned long)(max - 1));
        if (n <= 0) return -1;
        if (buf[n - 1] == '\n') n--;
        buf[n] = 0;
        return (int)n;
    }
    int len = 0, cur = 0, hb = sh_hist_n, stash_len = 0, have_stash = 0;
    char stash[LINE_MAX];
    for (;;) {
        char c;
        long r = uread(0, &c, 1);
        if (r <= 0) { if (len == 0) { usettermios(0, &saved); return -1; } continue; }
        if (c == '\r' || c == '\n') { uwrite(1, "\n", 1); buf[len] = 0; usettermios(0, &saved); return len; }
        if (c == 4) {                               /* ^D */
            if (len == 0) { usettermios(0, &saved); return -1; }
            if (cur < len) { for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1]; len--; refresh(pr, pl, buf, len, cur); }
            continue;
        }
        if (c == 127 || c == 8) { if (cur > 0) { for (int i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1]; cur--; len--; refresh(pr, pl, buf, len, cur); } continue; }
        if (c == 1) { cur = 0; refresh(pr, pl, buf, len, cur); continue; }   /* ^A */
        if (c == 5) { cur = len; refresh(pr, pl, buf, len, cur); continue; } /* ^E */
        if (c == 21) { len = 0; cur = 0; refresh(pr, pl, buf, len, cur); continue; } /* ^U */
        if (c == 11) { len = cur; refresh(pr, pl, buf, len, cur); continue; } /* ^K */
        if (c == 27) {                              /* ESC [ X */
            char a, b;
            if (uread(0, &a, 1) <= 0 || a != '[') continue;
            if (uread(0, &b, 1) <= 0) continue;
            if (b == 'A') {                         /* up: older history */
                if (hb > 0) {
                    if (hb == sh_hist_n) { for (int i = 0; i < len; i++) stash[i] = buf[i]; stash_len = len; have_stash = 1; }
                    hb--;
                    const char* h = sh_hist[hb % HIST_MAX]; int i = 0; while (h[i] && i < max - 1) { buf[i] = h[i]; i++; } len = cur = i;
                    refresh(pr, pl, buf, len, cur);
                }
            } else if (b == 'B') {                  /* down: newer history */
                if (hb < sh_hist_n) {
                    hb++;
                    int i = 0;
                    if (hb == sh_hist_n) { if (have_stash) for (; i < stash_len; i++) buf[i] = stash[i]; }
                    else { const char* h = sh_hist[hb % HIST_MAX]; while (h[i] && i < max - 1) { buf[i] = h[i]; i++; } }
                    len = cur = i;
                    refresh(pr, pl, buf, len, cur);
                }
            } else if (b == 'C') { if (cur < len) { cur++; refresh(pr, pl, buf, len, cur); } }
            else if (b == 'D') { if (cur > 0) { cur--; refresh(pr, pl, buf, len, cur); } }
            else if (b == '3') { char t; uread(0, &t, 1); if (cur < len) { for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1]; len--; refresh(pr, pl, buf, len, cur); } }
            continue;
        }
        if (c >= 32 && (unsigned char)c < 127) {    /* printable: insert at cursor */
            if (len < max - 1) { for (int i = len; i > cur; i--) buf[i] = buf[i - 1]; buf[cur] = c; len++; cur++; refresh(pr, pl, buf, len, cur); }
        }
    }
}

int umain(void) {
    char line[LINE_MAX];
    int  last = 0;
    int  shell_pgid = (int)ugetpgrp();

    for (;;) {
        int n = sh_readline("$ ", line, sizeof(line));
        if (n < 0) return last;                  /* EOF: leave with last status */
        line[n] = '\0';
        hist_push(line);

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
