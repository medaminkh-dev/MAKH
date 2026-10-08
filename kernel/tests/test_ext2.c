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

/* Write path (G2-c). Each write test only touches a file it creates itself, so
 * it never disturbs the read-only fixtures above (writes are ephemeral under
 * QEMU snapshot=on, but persist for the lifetime of the boot). */
KTEST(ext2, create_write_readback) {
    vnode_t* vn = vfs_create("/mnt/created.txt", VNODE_REG);
    KASSERT_TEST(vn != NULL);

    const int N = 15000;                        /* >12 KiB: forces single-indirect alloc */
    char* w = kmalloc(N);
    char* r = kmalloc(N);
    KASSERT_TEST(w != NULL && r != NULL);
    for (int i = 0; i < N; i++) w[i] = (char)('A' + (i % 26));

    KEXPECT_EQ((int)vfs_write(vn, w, N, 0), N);

    /* Re-resolve: a fresh vnode read straight from the on-disk inode/dirent. */
    vnode_t* vn2 = vfs_resolve("/mnt/created.txt");
    KASSERT_TEST(vn2 != NULL);
    KEXPECT_EQ((int)vn2->size, N);
    memset(r, 0, N);
    KEXPECT_EQ((int)vfs_read(vn2, r, N, 0), N);
    KEXPECT_EQ(memcmp(w, r, N), 0);

    kfree(w);
    kfree(r);
}

KTEST(ext2, overwrite_in_place) {
    vnode_t* vn = vfs_create("/mnt/ow.txt", VNODE_REG);
    KASSERT_TEST(vn != NULL);
    KEXPECT_EQ((int)vfs_write(vn, "abcdefghij", 10, 0), 10);
    KEXPECT_EQ((int)vfs_write(vn, "XY", 2, 2), 2);      /* overwrite bytes 2-3 */

    char buf[16];
    memset(buf, 0, sizeof(buf));
    vnode_t* vn2 = vfs_resolve("/mnt/ow.txt");
    KASSERT_TEST(vn2 != NULL);
    KEXPECT_EQ((int)vfs_read(vn2, buf, 10, 0), 10);
    KEXPECT_EQ(memcmp(buf, "abXYefghij", 10), 0);
}
