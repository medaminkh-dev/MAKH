/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* help: a short A-to-Z guide to MAKH OS and the commands available in /bin. */
#include "usys.h"

static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void put(const char* s) { uwrite(1, s, slen(s)); }

/* ANSI: bright cyan section headers, default body (the console understands SGR). */
#define HDR "\033[1;36m"
#define RST "\033[0m"

int umain(void) {
    put(HDR "MAKH OS" RST
        " - a small x86-64 operating system, built brick by brick.\n"
        "Running a user-space shell (ring 3) on MAKH's own kernel.\n\n");

    put(HDR "Shell" RST "\n"
        "  cd [dir]        change directory (builtin)\n"
        "  exit [code]     leave the shell (builtin)\n"
        "  cmd | cmd       pipelines; Ctrl+C interrupts, Ctrl+D is EOF\n"
        "  /path/to/prog   bare names are searched in /bin then /usr/bin\n\n");

    put(HDR "Files & directories" RST "\n"
        "  ls [-l] [-a] [path]   list (-l long/permissions, -a show dotfiles)\n"
        "  cat [file...]         print file(s) (or stdin)\n"
        "  pwd                   print the working directory\n"
        "  clear                 clear the screen\n"
        "  mkdir / rmdir / rm    create / remove directories / files\n"
        "  cp / mv / ln / touch  copy / move / link / create\n\n");

    put(HDR "Text" RST "\n"
        "  echo [words...]       print the arguments\n"
        "  grep / head / tail    search / first / last lines\n"
        "  wc / sort / cut       count / sort / select fields\n\n");

    put(HDR "System" RST "\n"
        "  whoami                print the user (MAKH is single-user: root)\n"
        "  id                    print uid/gid\n"
        "  uname [-a]            kernel name and version\n"
        "  env / date / sleep    environment / time / wait\n\n");

    put(HDR "Network" RST "\n"
        "  ping <ip> [count]     ICMP echo, with ttl and rtt min/avg/max\n"
        "  ifconfig              list the network interfaces\n\n");

    put(HDR "Develop (self-hosting)" RST "\n"
        "  tcc <file.c>          the Tiny C Compiler, running on MAKH\n"
        "  make                  build multi-file projects on MAKH\n\n");

    put("Type a command name to run it. More bricks are on the way.\n");
    return 0;
}
