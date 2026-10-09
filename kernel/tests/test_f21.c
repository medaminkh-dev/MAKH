/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_f21.c
 * F21 (self-hosting): tcc runs ON MAKH, compiles a freestanding C program to a
 * standard non-PIE ET_EXEC at 0x400000, and the higher-half kernel (Phase 21)
 * loads and runs the result. /bin/tcc and /share/tcc-hello.c ship in the
 * initrd; tcc writes the output into the root tmpfs. This is the first proof
 * that MAKH can build and run a program with a real compiler, on itself.
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
