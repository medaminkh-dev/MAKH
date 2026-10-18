/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * A tiny shell with job control and pipelines: a prompt that shows the working
 * directory, a line editor with history and TAB completion, redirection
 * (`>`, `>>`, `<`) and heredocs (`<<EOF`), and `|` pipelines run by
 * fork + dup2 + execve. A single command keeps full job control (its own
 * process group owns the terminal, so Ctrl+C hits it, not the shell). On EOF
 * (^D) the shell exits with the last command's status.
 */
#include "usys.h"

#define MAXARGS   16
#define MAXSTAGES 8
#define MAXNAME   64            /* VFS_NAME_MAX (63) + NUL                     */
#define DT_DIR    4             /* getdents64 d_type for a directory          */

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

/* ------------------------------------------------------------------ redirection
 * Pull `>`, `>>`, `<` (and their targets, whether "> f" or ">f") out of an argv,
 * in place. Returns 0, or -1 on a missing target. A heredoc (`<<`) is expanded
 * to a plain `< tmpfile` by the caller before tokenizing, so it arrives here as
 * an ordinary input redirection. */
typedef struct { const char* in; const char* out; int append; } redir_t;

static int strip_redirs(char** argv, redir_t* r) {
    r->in = r->out = 0; r->append = 0;
    int w = 0;
    for (int i = 0; argv[i]; i++) {
        char* a = argv[i];
        int is_out = (a[0] == '>'), is_in = (a[0] == '<');
        if (is_out || is_in) {
            int append = (is_out && a[1] == '>');
            const char* target = a + (append ? 2 : 1);
            if (!*target) {                       /* "> file": target is next arg */
                if (!argv[i + 1]) return -1;
                target = argv[++i];
            }
            if (is_out) { r->out = target; r->append = append; }
            else        { r->in = target; }
            continue;
        }
        argv[w++] = a;
    }
    argv[w] = 0;
    return 0;
}

/* In the child: open the redirection targets and dup them onto fd 0/1. Prints an
 * error and exits the child on failure. */
static void apply_redirs(const redir_t* r) {
    if (r->in) {
        long fd = uopen(r->in, O_RDONLY);
        if (fd < 0) { uwrite(2, "sh: ", 4); uwrite(2, r->in, sh_strlen(r->in)); uwrite(2, ": cannot open\n", 14); usyscall(SYS_EXIT, 1, 0, 0); }
        udup2((int)fd, 0); uclose((int)fd);
    }
    if (r->out) {
        int flags = O_WRONLY | O_CREAT | (r->append ? O_APPEND : O_TRUNC);
        long fd = uopen(r->out, flags);
        if (fd < 0) { uwrite(2, "sh: ", 4); uwrite(2, r->out, sh_strlen(r->out)); uwrite(2, ": cannot create\n", 16); usyscall(SYS_EXIT, 1, 0, 0); }
        udup2((int)fd, 1); uclose((int)fd);
    }
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

    redir_t rd;
    if (strip_redirs(argv, &rd) < 0) { uwrite(2, "sh: syntax error near redirection\n", 34); return 2; }
    if (!argv[0]) return 0;                        /* only a redirection, no cmd */

    long pid = ufork();
    if (pid < 0) return 127;
    if (pid == 0) {                              /* child: own group, exec    */
        usetpgid(0, 0);
        apply_redirs(&rd);
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
            if (tokenize(stages[i], argv) && argv[0]) {
                redir_t rd;                      /* explicit >,< override the pipe */
                if (strip_redirs(argv, &rd) == 0) { apply_redirs(&rd); if (argv[0]) sh_exec(argv); }
            }
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
 * An interactive editor with history and TAB completion, used when stdin is a
 * terminal. It puts the tty in raw mode (so it sees arrows, Ctrl-keys, Tab and
 * Delete), draws the line itself, and restores the tty before returning so
 * children run with a normal terminal. On a non-tty (a pipe or script) it falls
 * back to a plain read.
 *   Up/Down = history, Left/Right/Home(^A)/End(^E) = move, ^U kill line,
 *   ^K kill to end, Backspace/Delete, Tab = complete, ^D = EOF on an empty line.
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

/* --- TAB completion: gather candidate names and their longest common prefix --- */
typedef struct {
    int  count;                 /* candidates seen                              */
    int  lcp_len;               /* longest common prefix length                 */
    char lcp[MAXNAME];          /* longest common prefix of all candidates      */
    int  first_dir;             /* is the (single) candidate a directory?       */
    int  nnames;                /* names kept for listing (capped)              */
    char names[32][MAXNAME];
} comp_t;

static void comp_add(comp_t* c, const char* name, int is_dir) {
    int nl = 0; while (name[nl] && nl < MAXNAME - 1) nl++;
    if (c->count == 0) {
        for (int i = 0; i < nl; i++) c->lcp[i] = name[i];
        c->lcp[nl] = 0; c->lcp_len = nl; c->first_dir = is_dir;
    } else {
        int i = 0; while (i < c->lcp_len && name[i] && c->lcp[i] == name[i]) i++;
        c->lcp_len = i; c->lcp[i] = 0;
    }
    if (c->nnames < 32) {
        int i = 0; for (; name[i] && i < MAXNAME - 1; i++) c->names[c->nnames][i] = name[i];
        c->names[c->nnames][i] = 0; c->nnames++;
    }
    c->count++;
}

/* Walk `path`, feeding entries whose name starts with prefix[0..plen) to the
 * completion accumulator (skipping "." and ".."). */
static void scan_dir(const char* path, const char* prefix, int plen, comp_t* c) {
    long fd = uopen(path, O_RDONLY);
    if (fd < 0) return;
    char dbuf[2048]; long n;
    while ((n = ugetdents64((int)fd, dbuf, sizeof dbuf)) > 0) {
        long off = 0;
        while (off < n) {
            struct linux_dirent64* de = (struct linux_dirent64*)(dbuf + off);
            off += de->d_reclen;
            const char* name = de->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            int ok = 1;
            for (int i = 0; i < plen; i++) if (name[i] != prefix[i]) { ok = 0; break; }
            if (ok) comp_add(c, name, de->d_type == DT_DIR);
        }
    }
    uclose((int)fd);
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
        if (c == 9) {                               /* Tab: complete the token */
            int ts = cur; while (ts > 0 && buf[ts - 1] != ' ' && buf[ts - 1] != '|') ts--;
            int tlen = cur - ts;
            int cmdpos = 1;                         /* first word of a command?  */
            for (int i = ts - 1; i >= 0; i--) { if (buf[i] == ' ') continue; cmdpos = (buf[i] == '|'); break; }
            char tok[LINE_MAX];
            for (int i = 0; i < tlen; i++) tok[i] = buf[ts + i];
            tok[tlen] = 0;
            int has_slash = 0; for (int i = 0; i < tlen; i++) if (tok[i] == '/') has_slash = 1;
            comp_t cc; cc.count = cc.lcp_len = cc.nnames = cc.first_dir = 0;
            const char* base; int blen;
            if (cmdpos && !has_slash) {              /* command name: scan PATH dirs */
                base = tok; blen = tlen;
                scan_dir("/bin", base, blen, &cc);
                scan_dir("/usr/bin", base, blen, &cc);
            } else {                                 /* file/dir: split dir + base   */
                int slash = -1; for (int i = tlen - 1; i >= 0; i--) if (tok[i] == '/') { slash = i; break; }
                char dir[LINE_MAX];
                if (slash < 0) { dir[0] = '.'; dir[1] = 0; base = tok; blen = tlen; }
                else {
                    int j = 0; for (; j <= slash; j++) dir[j] = tok[j];
                    if (slash == 0) { dir[0] = '/'; dir[1] = 0; } else dir[slash + 1] = 0;
                    base = tok + slash + 1; blen = tlen - (slash + 1);
                }
                scan_dir(dir, base, blen, &cc);
            }
            if (cc.count > 0) {
                int add = cc.lcp_len - blen;         /* chars to insert past prefix */
                if (add > 0) {
                    for (int k = blen; k < cc.lcp_len && len < max - 1; k++) {
                        for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
                        buf[cur] = cc.lcp[k]; len++; cur++;
                    }
                }
                if (cc.count == 1) {                 /* unique: add '/' (dir) or ' ' */
                    char tail = cc.first_dir ? '/' : ' ';
                    if (len < max - 1) { for (int i = len; i > cur; i--) buf[i] = buf[i - 1]; buf[cur] = tail; len++; cur++; }
                    refresh(pr, pl, buf, len, cur);
                } else if (add > 0) {
                    refresh(pr, pl, buf, len, cur);  /* advanced the common prefix   */
                } else {                              /* ambiguous: list candidates   */
                    uwrite(1, "\n", 1);
                    for (int i = 0; i < cc.nnames; i++) { uwrite(1, cc.names[i], sh_strlen(cc.names[i])); uwrite(1, "  ", 2); }
                    uwrite(1, "\n", 1);
                    refresh(pr, pl, buf, len, cur);
                }
            }
            continue;
        }
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

/* Build the prompt: "makh:<cwd>$ " with the name in cyan and the path in green
 * (SGR escapes are zero-width, so the line editor's redraw math is unaffected).
 * Showing the cwd means the user never has to run pwd to know where they are. */
static void build_prompt(char* out, int max) {
    char cwd[128];
    long n = ugetcwd(cwd, sizeof cwd);
    if (n <= 0) { cwd[0] = '/'; cwd[1] = 0; }
    int w = 0;
    const char* pre = "\033[1;36mmakh\033[0m:\033[1;32m";
    for (const char* s = pre;  *s && w < max - 1; s++) out[w++] = *s;
    for (const char* s = cwd;  *s && w < max - 1; s++) out[w++] = *s;
    const char* post = "\033[0m$ ";
    for (const char* s = post; *s && w < max - 1; s++) out[w++] = *s;
    out[w] = 0;
}

/* If `line` contains a heredoc (`<<DELIM`), read the body from the terminal into
 * a temp file and rewrite the line as `... < /tmp/.heredoc ...`, so the normal
 * input-redirection path feeds the body to the command. Works before a pipe too
 * (`cat <<EOF | grep x`). Returns 0, or -1 if ^D ended the body early. */
static int expand_heredoc(char* line, int max) {
    int p = -1;
    for (int i = 0; line[i]; i++) if (line[i] == '<' && line[i + 1] == '<') { p = i; break; }
    if (p < 0) return 0;
    int d = p + 2; while (line[d] == ' ') d++;
    char delim[64]; int dl = 0;
    while (line[d] && line[d] != ' ' && dl < 63) delim[dl++] = line[d++];
    delim[dl] = 0;
    if (dl == 0) return 0;                          /* "<<" with no delimiter     */
    int after = d;

    const char* tmp = "/tmp/.heredoc";
    long fd = uopen(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { uwrite(2, "sh: heredoc: cannot create temp file\n", 37); return -1; }
    char hl[LINE_MAX];
    for (;;) {
        int n = sh_readline("> ", hl, sizeof hl);
        if (n < 0) { uclose((int)fd); return -1; }  /* ^D aborts the heredoc      */
        hl[n] = 0;
        if (sh_streq(hl, delim)) break;
        uwrite((int)fd, hl, (unsigned long)n);
        uwrite((int)fd, "\n", 1);
    }
    uclose((int)fd);

    char rebuilt[LINE_MAX]; int w = 0;
    for (int i = 0; i < p && w < max - 1; i++) rebuilt[w++] = line[i];
    for (const char* s = "< ";  *s && w < max - 1; s++) rebuilt[w++] = *s;
    for (const char* s = tmp;   *s && w < max - 1; s++) rebuilt[w++] = *s;
    for (int i = after; line[i] && w < max - 1; i++)    rebuilt[w++] = line[i];
    rebuilt[w] = 0;
    for (int i = 0; i <= w; i++) line[i] = rebuilt[i];
    return 0;
}

int umain(void) {
    char line[LINE_MAX];
    char prompt[192];
    int  last = 0;
    int  shell_pgid = (int)ugetpgrp();

    for (;;) {
        build_prompt(prompt, sizeof prompt);
        int n = sh_readline(prompt, line, sizeof(line));
        if (n < 0) return last;                  /* EOF: leave with last status */
        line[n] = '\0';
        hist_push(line);

        int blank = 1;
        for (int k = 0; line[k]; k++) if (line[k] != ' ') { blank = 0; break; }
        if (blank) continue;

        if (expand_heredoc(line, sizeof(line)) < 0) continue;  /* ^D during body */

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
