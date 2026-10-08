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

/* G2-d: mkdir, a file inside it, unlink the file, rmdir the directory. */
KTEST(ext2, mkdir_create_unlink_rmdir) {
    KEXPECT_EQ(vfs_mkdir("/mnt/d1"), 0);
    vnode_t* dv = vfs_resolve("/mnt/d1");
    KASSERT_TEST(dv != NULL);
    KEXPECT_EQ((int)dv->type, (int)VNODE_DIR);

    vnode_t* f = vfs_create("/mnt/d1/inside.txt", VNODE_REG);
    KASSERT_TEST(f != NULL);
    KEXPECT_EQ((int)vfs_write(f, "hi", 2, 0), 2);
    vnode_t* f2 = vfs_resolve("/mnt/d1/inside.txt");
    KASSERT_TEST(f2 != NULL);
    char b[4]; memset(b, 0, sizeof(b));
    KEXPECT_EQ((int)vfs_read(f2, b, 2, 0), 2);
    KEXPECT_EQ(memcmp(b, "hi", 2), 0);

    KEXPECT_EQ(vfs_unlink("/mnt/d1/inside.txt"), 0);
    KASSERT_TEST(vfs_resolve("/mnt/d1/inside.txt") == 0);
    KEXPECT_EQ(vfs_unlink("/mnt/d1"), 0);               /* now-empty dir (rmdir) */
    KASSERT_TEST(vfs_resolve("/mnt/d1") == 0);
}

/* G2-d: rename replaces the destination; the source name disappears. */
KTEST(ext2, rename_replaces) {
    vnode_t* a = vfs_create("/mnt/ra.txt", VNODE_REG);
    KASSERT_TEST(a != NULL);
    KEXPECT_EQ((int)vfs_write(a, "AAAA", 4, 0), 4);
    vnode_t* b = vfs_create("/mnt/rb.txt", VNODE_REG);
    KASSERT_TEST(b != NULL);
    KEXPECT_EQ((int)vfs_write(b, "B", 1, 0), 1);

    KEXPECT_EQ(vfs_rename("/mnt/ra.txt", "/mnt/rb.txt"), 0);
    KASSERT_TEST(vfs_resolve("/mnt/ra.txt") == 0);       /* source gone */
    vnode_t* r = vfs_resolve("/mnt/rb.txt");
    KASSERT_TEST(r != NULL);
    char rb[8]; memset(rb, 0, sizeof(rb));
    KEXPECT_EQ((int)vfs_read(r, rb, 7, 0), 4);           /* holds the moved bytes */
    KEXPECT_EQ(memcmp(rb, "AAAA", 4), 0);
}

/* G2-d: truncate frees blocks and resets the size. */
KTEST(ext2, truncate_to_zero) {
    vnode_t* v = vfs_create("/mnt/tr.txt", VNODE_REG);
    KASSERT_TEST(v != NULL);
    char* buf = kmalloc(5000);
    KASSERT_TEST(buf != NULL);
    memset(buf, 'Z', 5000);
    KEXPECT_EQ((int)vfs_write(v, buf, 5000, 0), 5000);
    KEXPECT_EQ((int)v->size, 5000);

    KASSERT_TEST(v->ops && v->ops->truncate);
    KEXPECT_EQ(v->ops->truncate(v, 0), 0);
    KEXPECT_EQ((int)v->size, 0);
    vnode_t* v2 = vfs_resolve("/mnt/tr.txt");
    KASSERT_TEST(v2 != NULL);
    KEXPECT_EQ((int)v2->size, 0);
    kfree(buf);
}
