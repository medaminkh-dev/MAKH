/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_g3.c
 * Phase 20-S (G3): the syscall surface a real toolchain needs, exercised by
 * real musl programs (not kernel stubs):
 *   - /bin/fio      opens, writes, reopens and reads a file on the on-disk
 *                   ext2 at /mnt — the libc file-I/O path down to a real disk;
 *   - /bin/envtest  re-execs itself with MAKH_G3=42 in envp and the child reads
 *                   it back with getenv() — proving execve() carries envp.
 * Each exits 42 iff its path worked end to end.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <proc.h>

KTEST(g3, musl_file_io_on_ext2) {
    int pid = proc_spawn_user("/bin/fio");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}

KTEST(g3, execve_passes_envp) {
    int pid = proc_spawn_user("/bin/envtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 42);
}
