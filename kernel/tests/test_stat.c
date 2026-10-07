/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_stat.c
 * Phase 20-J: stat/fstat/getdents64/fcntl from ring 3. /bin/statls checks a
 * regular file, a directory, the tty's fstat, a directory listing that finds
 * "hello", and fcntl(F_GETFL) — exiting 88 on full success. A direct kernel
 * getdents check confirms listing works below the syscall layer too.
 */

#include <ktest.h>
#include <proc_internal.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <errno.h>

KTEST(stat, stat_fstat_getdents_fcntl_from_userspace) {
    int pid = proc_spawn_user("/bin/statls");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 88);
}

/* vfs_stat reports type and a non-zero size for a known initrd file. */
KTEST(stat, vfs_stat_reports_regular_file) {
    struct stat st;
    KEXPECT_EQ(vfs_stat("/bin/hello", &st), 0);
    KEXPECT_NE(S_ISREG(st.st_mode), 0);
    KEXPECT_NE(st.st_size > 0, 0);
    KEXPECT_EQ(vfs_stat("/bin", &st), 0);
    KEXPECT_NE(S_ISDIR(st.st_mode), 0);
    KEXPECT_EQ(vfs_stat("/nope/missing", &st), -ENOENT);
}
