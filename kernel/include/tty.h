/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - tty.h
 * Terminal line discipline (Phase 19).
 *
 * The line discipline sits between raw input bytes and a reader. In canonical
 * mode it buffers a line, echoes it, and handles erase; with ISIG on, the
 * control characters raise signals on the terminal's foreground process group
 * (^C -> SIGINT, ^Z -> SIGTSTP); ^D ends a line / signals EOF. This is the path
 * by which Ctrl+C kills the running job.
 *
 * The rendering side still uses the existing VGA/serial console; a framebuffer
 * console with a PSF font and ANSI parsing is Phase 20.
 */

#ifndef MAKHOS_TTY_H
#define MAKHOS_TTY_H

#include <types.h>

/* c_lflag bits (subset of termios). */
#define ICANON   0x0002   /* canonical (line) mode     */
#define ECHO     0x0008   /* echo input                */
#define ISIG     0x0001   /* control chars raise signals */

/* c_cc indices. */
#define VINTR    0        /* ^C */
#define VQUIT    1        /* ^\ */
#define VERASE   2        /* backspace */
#define VEOF     3        /* ^D */
#define VSUSP    4        /* ^Z */
#define NCCS     8

typedef struct termios {
    uint32_t c_lflag;
    uint8_t  c_cc[NCCS];
} termios_t;

#define TTY_LINE_MAX 256

void      tty_init(void);
/* Feed one raw input byte through the line discipline. */
void      tty_input(char c);
/* Non-blocking read of up to n bytes of completed line data; returns count. */
long      tty_read(char* buf, size_t n);
/* 1 if a full line (or EOF) is available to read. */
int       tty_line_ready(void);

/* Foreground process group (tcsetpgrp/tcgetpgrp). */
void      tty_set_foreground(uint32_t pgid);
uint32_t  tty_get_foreground(void);

termios_t* tty_termios(void);

#endif /* MAKHOS_TTY_H */
