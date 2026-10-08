/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Scatter/gather I/O over a pipe: writev two chunks in, readv them back into
 * two buffers, verify the bytes reassembled correctly. Exit 44 on success.
 * This is the stdio path musl flushes through. */
#include "usys.h"

int umain(void) {
    int fds[2];
    if (upipe(fds) < 0) return 1;

    struct iovec w[2] = { { "ab", 2 }, { "cde", 3 } };
    if (uwritev(fds[1], w, 2) != 5) return 2;

    char b1[2] = {0}, b2[3] = {0};
    struct iovec r[2] = { { b1, 2 }, { b2, 3 } };
    if (ureadv(fds[0], r, 2) != 5) return 3;

    if (b1[0] != 'a' || b1[1] != 'b') return 4;
    if (b2[0] != 'c' || b2[1] != 'd' || b2[2] != 'e') return 5;

    return 44;
}
