/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/*
 * MakhOS Phase 20-S (G3): a real musl program doing file I/O on the on-disk
 * ext2 filesystem mounted at /mnt. Create a file, write to it, close, reopen,
 * read it back, and exit 42 iff the bytes round-tripped — the libc open/write/
 * read/close path all the way down to ext2 on a virtio-blk disk.
 */
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    const char* m = "musl file IO on ext2\n";
    long len = (long)strlen(m);

    int fd = open("/mnt/g3.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) return 1;
    if (write(fd, m, (size_t)len) != len) return 2;
    close(fd);

    char buf[64];
    memset(buf, 0, sizeof(buf));
    int fd2 = open("/mnt/g3.txt", O_RDONLY);
    if (fd2 < 0) return 3;
    long n = read(fd2, buf, sizeof(buf) - 1);
    close(fd2);
    if (n != len) return 4;
    if (strcmp(buf, m) != 0) return 5;
    return 42;
}
