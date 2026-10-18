/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * edit FILE — a small full-screen text editor for MAKH (a tiny nano).
 * Runs in ring 3 on the framebuffer console in raw mode. Keys:
 *   arrows        move            Ctrl-S  save
 *   Enter         split line      Ctrl-Q  quit (Ctrl-X also quits)
 *   Backspace     delete left     Ctrl-A/E  line start/end
 *   printable     insert
 * This is MAKH's own editor: a line-array model, redrawn each keystroke. It
 * assumes an 80x25 console (no window-size query yet) and truncates the view at
 * 80 columns; files scroll vertically.
 */
#include "usys.h"

#define ROWS      25
#define COLS      80
#define TEXTROWS  (ROWS - 1)        /* last row is the status bar            */
#define MAXLINES  1024
#define MAXCOL    1024

static char line[MAXLINES][MAXCOL];
static int  llen[MAXLINES];
static int  nlines;

static int  cx, cy;                 /* cursor in text coords (col, line)     */
static int  top;                    /* first visible line                    */
static int  dirty;
static const char* fname;

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void out(const char* s){ uwrite(1, s, slen(s)); }

/* write a decimal number */
static void outn(int v){
    char b[12]; int n=0; if(v<0)v=0;
    if(!v){ uwrite(1,"0",1); return; }
    while(v){ b[n++]=(char)('0'+v%10); v/=10; }
    for(int i=0;i<n/2;i++){ char t=b[i]; b[i]=b[n-1-i]; b[n-1-i]=t; }
    uwrite(1,b,(unsigned long)n);
}

/* move the hardware cursor to 1-based (row,col) */
static void gotoxy(int row, int col){ out("\033["); outn(row); out(";"); outn(col); out("H"); }

static void load(const char* path){
    nlines = 0; llen[0] = 0; line[0][0] = 0;
    int fd = (int)uopen(path, O_RDONLY);
    if (fd < 0) { nlines = 1; llen[0] = 0; return; }   /* new file */
    char rb[1024]; long n; int L = 0, c = 0;
    while ((n = uread(fd, rb, sizeof rb)) > 0) {
        for (long i = 0; i < n; i++) {
            char ch = rb[i];
            if (ch == '\n') { llen[L] = c; line[L][c] = 0; if (L < MAXLINES-1) L++; c = 0; line[L][0]=0; }
            else if (c < MAXCOL - 1) line[L][c++] = ch;
        }
    }
    llen[L] = c; line[L][c] = 0;
    nlines = L + 1;
    uclose(fd);
}

static int save(void){
    int fd = (int)uopen(fname, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    for (int i = 0; i < nlines; i++) {
        if (llen[i]) uwrite(fd, line[i], (unsigned long)llen[i]);
        if (i < nlines - 1) uwrite(fd, "\n", 1);
    }
    uclose(fd);
    dirty = 0;
    return 0;
}

static void draw(void){
    out("\033[H");                                     /* home */
    for (int r = 0; r < TEXTROWS; r++) {
        int li = top + r;
        gotoxy(r + 1, 1);
        out("\033[K");
        if (li < nlines) {
            int w = llen[li] < COLS ? llen[li] : COLS;
            uwrite(1, line[li], (unsigned long)w);
        }
    }
    /* status bar */
    gotoxy(ROWS, 1);
    out("\033[K\033[7m");                               /* reverse video */
    out(" edit: "); out(fname); out(dirty ? " [modified]" : "");
    out("  ^S save  ^Q quit  ");
    out("\033[0m");
    gotoxy(cy - top + 1, cx + 1);                       /* cursor */
}

static void scroll_fix(void){
    if (cy < top) top = cy;
    if (cy >= top + TEXTROWS) top = cy - TEXTROWS + 1;
    if (top < 0) top = 0;
}

int umain(int argc, char** argv){
    if (argc < 2) { uwrite(2, "usage: edit FILE\n", 17); return 2; }
    fname = argv[1];

    struct termios saved;
    if (ugettermios(0, &saved) < 0) { uwrite(2, "edit: not a terminal\n", 21); return 1; }
    struct termios raw = saved; raw.c_lflag &= ~(unsigned)(ICANON | ECHO); usettermios(0, &raw);

    load(fname);
    cx = cy = top = dirty = 0;
    out("\033[2J");
    draw();

    for (;;) {
        char c;
        if (uread(0, &c, 1) <= 0) continue;

        if (c == 17 || c == 24) break;                  /* ^Q / ^X: quit */
        if (c == 19) { save(); draw(); continue; }      /* ^S: save */
        if (c == 1) { cx = 0; draw(); continue; }       /* ^A */
        if (c == 5) { cx = llen[cy]; draw(); continue; }/* ^E */

        if (c == '\r' || c == '\n') {                   /* split line at cursor */
            if (nlines < MAXLINES - 1) {
                for (int i = nlines; i > cy + 1; i--) { for (int k=0;k<=llen[i-1];k++) line[i][k]=line[i-1][k]; llen[i]=llen[i-1]; }
                int tail = llen[cy] - cx;
                for (int k = 0; k < tail; k++) line[cy+1][k] = line[cy][cx+k];
                line[cy+1][tail] = 0; llen[cy+1] = tail;
                line[cy][cx] = 0; llen[cy] = cx;
                nlines++; cy++; cx = 0; dirty = 1;
            }
            scroll_fix(); draw(); continue;
        }
        if (c == 127 || c == 8) {                       /* backspace */
            if (cx > 0) {
                for (int k = cx - 1; k < llen[cy] - 1; k++) line[cy][k] = line[cy][k+1];
                llen[cy]--; cx--; line[cy][llen[cy]] = 0; dirty = 1;
            } else if (cy > 0) {                        /* join with previous */
                int p = cy - 1, pl = llen[p];
                if (pl + llen[cy] < MAXCOL - 1) {
                    for (int k = 0; k < llen[cy]; k++) line[p][pl+k] = line[cy][k];
                    llen[p] += llen[cy]; line[p][llen[p]] = 0;
                    for (int i = cy; i < nlines - 1; i++) { for (int k=0;k<=llen[i+1];k++) line[i][k]=line[i+1][k]; llen[i]=llen[i+1]; }
                    nlines--; cy = p; cx = pl; dirty = 1;
                }
            }
            scroll_fix(); draw(); continue;
        }
        if (c == 27) {                                  /* ESC [ X : arrows */
            char a, b;
            if (uread(0, &a, 1) <= 0 || a != '[') continue;
            if (uread(0, &b, 1) <= 0) continue;
            if (b == 'A') { if (cy > 0) cy--; }
            else if (b == 'B') { if (cy < nlines - 1) cy++; }
            else if (b == 'C') { if (cx < llen[cy]) cx++; else if (cy < nlines-1) { cy++; cx=0; } }
            else if (b == 'D') { if (cx > 0) cx--; else if (cy > 0) { cy--; cx=llen[cy]; } }
            else if (b == '3') { char t; uread(0,&t,1);   /* Delete: remove char at cursor */
                if (cx < llen[cy]) { for (int k=cx;k<llen[cy]-1;k++) line[cy][k]=line[cy][k+1]; llen[cy]--; line[cy][llen[cy]]=0; dirty=1; } }
            if (cx > llen[cy]) cx = llen[cy];           /* clamp to line */
            scroll_fix(); draw(); continue;
        }
        if (c >= 32 && (unsigned char)c < 127) {        /* insert printable */
            if (llen[cy] < MAXCOL - 1) {
                for (int k = llen[cy]; k > cx; k--) line[cy][k] = line[cy][k-1];
                line[cy][cx] = c; llen[cy]++; cx++; line[cy][llen[cy]] = 0; dirty = 1;
            }
            draw(); continue;
        }
    }

    usettermios(0, &saved);
    out("\033[2J\033[H");                               /* leave a clean screen */
    return 0;
}
