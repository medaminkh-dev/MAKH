/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - tty/tty.c
 * Terminal line discipline. See tty.h.
 */

#include <tty.h>
#include <signal.h>
#include <sched.h>
#include <irq.h>
#include <errno.h>
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

/* Non-canonical (raw) input: a byte ring, so an interactive line editor in a
 * ring-3 shell can read keystrokes one at a time (VMIN=1 style) without the IRQ
 * producer and the read() consumer clobbering a shared line buffer. */
#define TTY_RAW_Q 256
static char      raw_q[TTY_RAW_Q];
static size_t    raw_head, raw_tail;

static wait_queue_t tty_wq;            /* readers blocked waiting for a line    */
static int          tty_active;        /* 1 => keyboard feeds the line discipline */

void tty_set_active(int on) { tty_active = on; }
int  tty_is_active(void)    { return tty_active; }

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
    raw_head = raw_tail = 0;
    tty_wq.head = tty_wq.tail = 0;
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
    wq_wake_all(&tty_wq);           /* a blocked read() can now proceed */
}

void tty_input(char c) {
    /* Signal-generating control characters (ISIG). */
    if (termios.c_lflag & ISIG) {
        if (c == (char)termios.c_cc[VINTR]) { signal_send_pgrp(fg_pgid, SIGINT);  return; }
        if (c == (char)termios.c_cc[VSUSP]) { signal_send_pgrp(fg_pgid, SIGTSTP); return; }
        if (c == (char)termios.c_cc[VQUIT]) { signal_send_pgrp(fg_pgid, SIGQUIT); return; }
    }

    if (!(termios.c_lflag & ICANON)) {        /* raw mode: one byte at a time */
        size_t nh = (raw_head + 1) % TTY_RAW_Q;
        if (nh != raw_tail) { raw_q[raw_head] = c; raw_head = nh; }  /* drop on overflow */
        echo(c);
        wq_wake_all(&tty_wq);
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

int tty_line_ready(void) {
    if (!(termios.c_lflag & ICANON))
        return raw_head != raw_tail || (ready_has && ready_len) || ready_eof;
    return ready_has || ready_eof;
}

long tty_read(char* buf, size_t n) {
    if (!(termios.c_lflag & ICANON)) {        /* raw: drain whatever bytes we have */
        size_t k = 0;
        while (k < n && raw_tail != raw_head) {
            buf[k++] = raw_q[raw_tail];
            raw_tail = (raw_tail + 1) % TTY_RAW_Q;
        }
        if (k == 0) {
            /* Bridge: input assembled in canonical mode (e.g. before the shell
             * switched to raw for line editing) is still delivered byte-wise. */
            if (ready_has && ready_len > 0) {
                size_t avail = ready_len < n ? ready_len : n;
                memcpy(buf, ready, avail);
                memmove(ready, ready + avail, ready_len - avail);
                ready_len -= avail;
                k = avail;
                if (ready_len == 0) ready_has = 0;
            } else if (ready_eof) {
                ready_eof = 0;                /* deliver EOF (0 bytes) */
            }
        }
        return (long)k;
    }
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

/* Blocking read: wait until a line (or EOF) is available, then return it. This
 * is the path a user process's read() on the controlling terminal takes.
 * Returns the byte count (0 = EOF), or -EINTR if a signal woke the waiter. */
long tty_read_blocking(char* buf, size_t n) {
    for (;;) {
        irqflags_t f = local_irq_save();
        if (tty_line_ready()) {
            long k = tty_read(buf, n);
            local_irq_restore(f);
            return k;
        }
        int rc = sched_wait_event(&tty_wq, 0, f);   /* sleeps; restores f */
        if (rc == 2) return -EINTR;                 /* a signal interrupted us */
    }
}
