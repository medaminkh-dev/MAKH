/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - tty/tty.c
 * Terminal line discipline. See tty.h.
 */

#include <tty.h>
#include <signal.h>
#include <vga.h>
#include <lib/string.h>

static termios_t termios;
static uint32_t  fg_pgid;

static char      line[TTY_LINE_MAX];   /* current (incomplete) canonical line */
static size_t    line_len;

static char      ready[TTY_LINE_MAX];  /* completed line, waiting for a reader */
static size_t    ready_len;
static int       ready_has;            /* a line (or EOF) is available         */
static int       ready_eof;

void tty_init(void) {
    termios.c_lflag = ICANON | ECHO | ISIG;
    termios.c_cc[VINTR]  = 3;    /* ^C  */
    termios.c_cc[VQUIT]  = 28;   /* ^\  */
    termios.c_cc[VERASE] = 127;  /* DEL */
    termios.c_cc[VEOF]   = 4;    /* ^D  */
    termios.c_cc[VSUSP]  = 26;   /* ^Z  */
    fg_pgid = 0;
    line_len = ready_len = 0;
    ready_has = ready_eof = 0;
}

termios_t* tty_termios(void) { return &termios; }

void tty_set_foreground(uint32_t pgid) { fg_pgid = pgid; }
uint32_t tty_get_foreground(void) { return fg_pgid; }

static void echo(char c) {
    if (termios.c_lflag & ECHO) terminal_putchar(c);
}

static void line_complete(void) {
    memcpy(ready, line, line_len);
    ready_len = line_len;
    ready_has = 1;
    line_len = 0;
}

void tty_input(char c) {
    /* Signal-generating control characters (ISIG). */
    if (termios.c_lflag & ISIG) {
        if (c == (char)termios.c_cc[VINTR]) { signal_send_pgrp(fg_pgid, SIGINT);  return; }
        if (c == (char)termios.c_cc[VSUSP]) { signal_send_pgrp(fg_pgid, SIGTSTP); return; }
        if (c == (char)termios.c_cc[VQUIT]) { signal_send_pgrp(fg_pgid, SIGQUIT); return; }
    }

    if (!(termios.c_lflag & ICANON)) {        /* raw mode: pass bytes straight through */
        if (line_len < TTY_LINE_MAX) line[line_len++] = c;
        echo(c);
        if (line_len == TTY_LINE_MAX) line_complete();
        return;
    }

    /* Canonical mode. */
    if (c == (char)termios.c_cc[VEOF]) {       /* ^D: deliver what we have as EOF */
        line_complete();
        if (ready_len == 0) ready_eof = 1;
        return;
    }
    if (c == (char)termios.c_cc[VERASE] || c == '\b') {
        if (line_len > 0) {
            line_len--;
            if (termios.c_lflag & ECHO) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
        }
        return;
    }
    if (c == '\n' || c == '\r') {
        if (line_len < TTY_LINE_MAX) line[line_len++] = '\n';
        echo('\n');
        line_complete();
        return;
    }
    if (line_len < TTY_LINE_MAX - 1) {
        line[line_len++] = c;
        echo(c);
    }
}

int tty_line_ready(void) { return ready_has || ready_eof; }

long tty_read(char* buf, size_t n) {
    if (ready_eof && ready_len == 0) { ready_eof = 0; return 0; }   /* EOF */
    if (!ready_has) return 0;                                       /* nothing yet */
    size_t k = ready_len < n ? ready_len : n;
    memcpy(buf, ready, k);
    /* Shift any remainder (a short read leaves the rest for next time). */
    memmove(ready, ready + k, ready_len - k);
    ready_len -= k;
    if (ready_len == 0) { ready_has = 0; ready_eof = 0; }
    return (long)k;
}
