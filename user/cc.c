/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * cc — a friendly C compiler driver for MAKH. The bundled TinyCC (/bin/tcc.real)
 * is a fine compiler, but on its own it does not know where MAKH keeps the musl
 * sysroot, so a plain "tcc file.c" fails with "crt1.o not found". This driver
 * (installed as both /bin/cc and /bin/tcc) fills in the musl link line so a
 * one-liner just works:
 *
 *   cc hello.c            ->  ./hello
 *   cc -o prog hello.c    ->  ./prog
 *
 * It expands to the same command the self-host demo uses:
 *   tcc -nostdinc -I/usr/include -nostdlib -static -L/usr/lib \
 *       /usr/lib/crt1.o /usr/lib/crti.o <args> \
 *       /usr/lib/crtn.o -lc /usr/lib/libtcc1.a -o <out>
 *
 * If you pass your own toolchain flags (-c, -nostdlib, -I, -L, -B, -E, -S,
 * -run, -print-search-dirs, -v), the driver steps aside and runs the real tcc
 * unchanged — so advanced use and the self-hosting build keep working.
 */
#include "usys.h"

#define REAL "/bin/tcc.real"

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static void eput(const char* s){ uwrite(2, s, slen(s)); }
static int streq(const char* a, const char* b){ while(*a&&*a==*b){a++;b++;} return *a==*b; }
static int pre(const char* s, const char* p){ while(*p){ if(*s++!=*p++) return 0; } return 1; }
static int ends_c(const char* s){ unsigned long n=slen(s); return n>2 && s[n-2]=='.' && s[n-1]=='c'; }

/* argv[0..] plus envp from the SysV stack (argv[argc]=NULL, env follows). */
int umain(int argc, char** argv){
    char** envp = argv + argc + 1;

    /* Does the user drive the toolchain themselves? Then don't interfere. */
    int advanced = 0;
    for (int i = 1; i < argc; i++){
        const char* a = argv[i];
        if (a[0] != '-') continue;
        if (streq(a,"-c")||streq(a,"-nostdlib")||streq(a,"-nostdinc")||streq(a,"-E")||
            streq(a,"-S")||streq(a,"-run")||streq(a,"-B")||streq(a,"-v")||
            streq(a,"-print-search-dirs")||streq(a,"--help")||pre(a,"-I")||pre(a,"-L"))
            advanced = 1;
    }
    if (advanced){
        argv[0] = REAL;
        uexecve(REAL, argv, envp);
        eput("cc: cannot run " REAL "\n");
        return 127;
    }

    /* Friendly mode: find -o and pick a default output name from the first .c. */
    const char* out = 0;
    const char* firstc = 0;
    for (int i = 1; i < argc; i++){
        if (streq(argv[i], "-o") && i+1 < argc) { out = argv[i+1]; i++; continue; }
        if (!firstc && ends_c(argv[i])) firstc = argv[i];
    }
    static char outbuf[128];
    if (!out){
        if (firstc){                              /* strip the dir prefix and ".c" */
            const char* b = firstc;
            for (const char* p = firstc; *p; p++) if (*p=='/') b = p+1;
            int blen = (int)slen(b), j = 0;
            for (; j < blen-2 && j < 126; j++) outbuf[j] = b[j];
            outbuf[j] = 0;
            out = outbuf;
        } else {
            out = "a.out";
        }
    }

    /* Build: REAL -nostdinc -I/usr/include -nostdlib -static -L/usr/lib
     *        /usr/lib/crt1.o /usr/lib/crti.o <user args minus -o pair>
     *        /usr/lib/crtn.o -lc /usr/lib/libtcc1.a -o <out>  */
    char* nv[96];
    int n = 0;
    nv[n++] = "tcc";
    nv[n++] = "-nostdinc";
    nv[n++] = "-I/usr/include";
    nv[n++] = "-nostdlib";
    nv[n++] = "-static";
    nv[n++] = "-L/usr/lib";
    nv[n++] = "/usr/lib/crt1.o";
    nv[n++] = "/usr/lib/crti.o";
    for (int i = 1; i < argc && n < 86; i++){
        if (streq(argv[i], "-o") && i+1 < argc) { i++; continue; }   /* we re-add -o */
        nv[n++] = argv[i];
    }
    nv[n++] = "/usr/lib/crtn.o";
    nv[n++] = "-lc";
    nv[n++] = "/usr/lib/libtcc1.a";
    nv[n++] = "-o";
    nv[n++] = (char*)out;
    nv[n] = 0;

    uexecve(REAL, nv, envp);
    eput("cc: cannot run " REAL "\n");
    return 127;
}
