/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_f21.c
 * F21 (self-hosting): tcc runs ON MAKH, compiles C to a standard non-PIE
 * ET_EXEC at 0x400000, and the higher-half kernel (Phase 21) loads and runs
 * the result. Two bricks:
 *   F21-a: a freestanding program (its own _start + inline syscalls, -nostdlib);
 *   F21-b: a real libc program (<stdio.h>/<stdlib.h>/<string.h>, printf/malloc)
 *          linked against a musl sysroot (headers + libc.a + crt + libtcc1.a)
 *          staged in the initrd — MAKH compiling and running a program that
 *          rides a full C library, on itself.
 *   F21-c: GNU make (a static-musl prebuilt) drives tcc across a multi-file
 *          project (/share/mkproj) — two objects compiled and linked into one
 *          executable, a real build system running on MAKH.
 * /bin/tcc, /bin/make, /share/tcc-*.c, /share/mkproj and /usr (the sysroot)
 * ship in the initrd; the tools write their output into the root tmpfs. This is
 * the proof that MAKH can build and run programs with a real toolchain, on
 * itself.
 */
#include <ktest.h>
#include <proc_internal.h>
#include <proc.h>

KTEST(f21, tcc_compiles_and_runs_on_makh) {
    /* tcc -nostdlib -static -o /tmp/tcc-out /share/tcc-hello.c */
    char* const argv[] = {
        "tcc", "-nostdlib", "-static",
        "-o", "/tmp/tcc-out", "/share/tcc-hello.c", 0
    };
    int pid = proc_spawn_user_argv("/bin/tcc", argv);
    KASSERT_TEST(pid > 0);
    int st = -1;
    sys_waitpid(pid, &st);
    KEXPECT_EQ(st, 0);                  /* tcc compiled + linked successfully */

    /* Run exactly what tcc just emitted. */
    int pid2 = proc_spawn_user("/tmp/tcc-out");
    KASSERT_TEST(pid2 > 0);
    int st2 = -1;
    sys_waitpid(pid2, &st2);
    KEXPECT_EQ(st2, 42);                /* the freshly compiled program ran */
}

KTEST(f21, tcc_compiles_libc_program_on_makh) {
    /* F21-b: tcc links a real <stdio.h>/<stdlib.h>/<string.h> program against
     * the musl sysroot staged under /usr, exactly as a host cross-compile would:
     *   tcc -nostdinc -nostdlib -static -I/usr/include \
     *       /usr/lib/crt1.o /usr/lib/crti.o /share/tcc-full.c /usr/lib/crtn.o \
     *       -L/usr/lib -lc /usr/lib/libtcc1.a -o /tmp/full
     * -nostdinc/-nostdlib hand tcc only MAKH's sysroot (never any host paths);
     * the crt files and -lc bring up musl's __libc_start_main on MAKH's syscall
     * surface; libtcc1.a supplies tcc's compiler runtime helpers. The output is
     * a static non-PIE ET_EXEC at 0x400000 the higher-half kernel loads. */
    char* const argv[] = {
        "tcc", "-nostdinc", "-nostdlib", "-static",
        "-I/usr/include",
        "/usr/lib/crt1.o", "/usr/lib/crti.o",
        "/share/tcc-full.c",
        "/usr/lib/crtn.o",
        "-L/usr/lib", "-lc", "/usr/lib/libtcc1.a",
        "-o", "/tmp/full", 0
    };
    int pid = proc_spawn_user_argv("/bin/tcc", argv);
    KASSERT_TEST(pid > 0);
    int st = -1;
    sys_waitpid(pid, &st);
    KEXPECT_EQ(st, 0);                  /* tcc compiled + linked against musl */

    /* Run the freshly built libc program: musl's crt starts it up, it
     * malloc()s, strcpy()s, printf()s and exits 42. */
    int pid2 = proc_spawn_user("/tmp/full");
    KASSERT_TEST(pid2 > 0);
    int st2 = -1;
    sys_waitpid(pid2, &st2);
    KEXPECT_EQ(st2, 42);                /* the libc program ran on MAKH */
}

KTEST(f21, make_builds_multifile_project_on_makh) {
    /* F21-c: GNU make drives tcc across the /share/mkproj project — two
     * translation units (main.c + greet.c) compiled to objects and linked with
     * musl's crt + libc into one executable. The Makefile's recipes are plain
     * absolute-path tcc commands, so make runs them directly (no /bin/sh). */
    char* const argv[] = { "make", "-C", "/share/mkproj", 0 };
    int pid = proc_spawn_user_argv("/bin/make", argv);
    KASSERT_TEST(pid > 0);
    int st = -1;
    sys_waitpid(pid, &st);
    KEXPECT_EQ(st, 0);                  /* make built the project (tcc exit 0) */

    /* Run what make just built: it prints its line via musl and exits 42. */
    int pid2 = proc_spawn_user("/share/mkproj/app");
    KASSERT_TEST(pid2 > 0);
    int st2 = -1;
    sys_waitpid(pid2, &st2);
    KEXPECT_EQ(st2, 42);               /* the make-built multi-file binary ran */
}
