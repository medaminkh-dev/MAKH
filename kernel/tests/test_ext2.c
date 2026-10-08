/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_ext2.c
 * Phase 20-Q (G2-b): a read-only ext2 filesystem on a virtio-blk disk, mounted
 * into the VFS at /mnt. The tests open real on-disk files through the ordinary
 * vfs_resolve/vfs_read path:
 *   - /mnt/hello.txt      -> exact small-file contents (direct block);
 *   - /mnt/dir/nested.txt -> a file reached through a subdirectory lookup;
 *   - /mnt/big.txt        -> a 20000-byte file whose tail lives past the 12
 *     direct blocks, so reading it correctly exercises the single-indirect map.
 * The image is tools/mkext2.sh, attached as a second virtio-blk disk.
 */

#include <ktest.h>
#include <fs/vfs.h>
#include <mm/kheap.h>
#include <lib/string.h>

KTEST(ext2, reads_a_small_file) {
    vnode_t* vn = vfs_resolve("/mnt/hello.txt");
    KASSERT_TEST(vn != NULL);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    long r = vfs_read(vn, buf, sizeof(buf) - 1, 0);
    KEXPECT_EQ((int)r, 19);
    KEXPECT_EQ(memcmp(buf, "ext2 works on MAKH\n", 19), 0);
}

KTEST(ext2, lookup_through_subdirectory) {
    vnode_t* vn = vfs_resolve("/mnt/dir/nested.txt");
    KASSERT_TEST(vn != NULL);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    long r = vfs_read(vn, buf, sizeof(buf) - 1, 0);
    KEXPECT_EQ((int)r, 10);
    KEXPECT_EQ(memcmp(buf, "nested-ok\n", 10), 0);
}

KTEST(ext2, reads_through_indirect_block) {
    vnode_t* vn = vfs_resolve("/mnt/big.txt");
    KASSERT_TEST(vn != NULL);
    KEXPECT_EQ((int)vn->size, 20000);

    char* buf = kmalloc(20000);
    KASSERT_TEST(buf != NULL);
    long r = vfs_read(vn, buf, 20000, 0);
    KEXPECT_EQ((int)r, 20000);

    /* byte N == '0' + (N % 10); sample across the file, including well past the
     * 12 KiB covered by direct blocks (so the indirect map had to be right). */
    int ok = 1;
    for (int off = 0; off < 20000; off += 503)
        if (buf[off] != (char)('0' + (off % 10))) ok = 0;
    if (buf[15000] != (char)('0' + (15000 % 10))) ok = 0;   /* past block 12 */
    if (buf[19999] != (char)('0' + (19999 % 10))) ok = 0;   /* last byte */
    KEXPECT_EQ(ok, 1);

    kfree(buf);
}
