/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_iov.c
 * Phase F20-a: scatter/gather I/O. /bin/iovtest writev's two chunks into a pipe
 * and readv's them back into two buffers, exiting 44 iff the bytes reassembled
 * correctly — the buffered-stdio path musl flushes through.
 */

#include <ktest.h>
#include <proc_internal.h>

KTEST(iov, writev_readv_over_a_pipe) {
    int pid = proc_spawn_user("/bin/iovtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 44);
}
