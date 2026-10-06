/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_vfs.c
 * Phase 18: the virtual filesystem, tmpfs, devfs, path lookup, the fd layer,
 * and the initrd. Everything runs against the live root filesystem in one boot.
 */

#include <ktest.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/kheap.h>

KTEST(vfs, create_write_read_close) {
    int fd = vfs_open("/scratch.txt", O_CREAT | O_RDWR);
    KASSERT_TEST(fd >= 0);
    KEXPECT_EQ(vfs_fd_write(fd, "hello world", 11), 11);
    KEXPECT_EQ(vfs_lseek(fd, 0, SEEK_SET), 0);
    char buf[32];
    memset(buf, 0, sizeof(buf));
    KEXPECT_EQ(vfs_fd_read(fd, buf, sizeof(buf)), 11);
    KEXPECT_EQ(memcmp(buf, "hello world", 11), 0);
    KEXPECT_EQ(vfs_close(fd), 0);

    /* Re-open: the data persists in the tmpfs vnode. */
    fd = vfs_open("/scratch.txt", O_RDONLY);
    KASSERT_TEST(fd >= 0);
    KEXPECT_EQ(vfs_fd_read(fd, buf, 5), 5);
    KEXPECT_EQ(memcmp(buf, "hello", 5), 0);
    vfs_close(fd);
}

KTEST(vfs, directories_and_path_lookup) {
    KEXPECT_EQ(vfs_mkdir("/a"), 0);
    KEXPECT_EQ(vfs_mkdir("/a/b"), 0);
    vnode_t* vn = vfs_create("/a/b/deep.txt", VNODE_REG);
    KASSERT_TEST(vn != (void*)0);
    KEXPECT_EQ(vfs_write(vn, "x", 1, 0), 1);
    KEXPECT(vfs_resolve("/a") != (void*)0);
    KEXPECT(vfs_resolve("/a/b") != (void*)0);
    KEXPECT(vfs_resolve("/a/b/deep.txt") == vn);
    KEXPECT(vfs_resolve("/a/b/missing") == (void*)0);
    KEXPECT(vfs_resolve("/nope/x") == (void*)0);
}

KTEST(vfs, append_and_lseek_grow) {
    int fd = vfs_open("/log", O_CREAT | O_WRONLY | O_APPEND);
    KASSERT_TEST(fd >= 0);
    vfs_fd_write(fd, "AAA", 3);
    vfs_fd_write(fd, "BBB", 3);
    vfs_close(fd);
    fd = vfs_open("/log", O_RDONLY);
    char b[8]; memset(b, 0, sizeof(b));
    KEXPECT_EQ(vfs_fd_read(fd, b, 6), 6);
    KEXPECT_EQ(memcmp(b, "AAABBB", 6), 0);
    vfs_close(fd);
}

KTEST(vfs, unlink_removes_file) {
    vfs_create("/temp", VNODE_REG);
    KEXPECT(vfs_resolve("/temp") != (void*)0);
    KEXPECT_EQ(vfs_unlink("/temp"), 0);
    KEXPECT(vfs_resolve("/temp") == (void*)0);
    KEXPECT(vfs_open("/temp", O_RDONLY) < 0);
}

KTEST(vfs, readdir_lists_children) {
    vfs_mkdir("/rd");
    vfs_create("/rd/one", VNODE_REG);
    vfs_create("/rd/two", VNODE_REG);
    vnode_t* dir = vfs_resolve("/rd");
    KASSERT_TEST(dir != (void*)0 && dir->ops->readdir);
    int seen = 0;
    char name[VFS_NAME_MAX + 1];
    for (uint32_t i = 0; dir->ops->readdir(dir, i, name) == 0; i++) {
        if (!strcmp(name, "one") || !strcmp(name, "two")) seen++;
    }
    KEXPECT_EQ(seen, 2);
}

KTEST(vfs, devfs_nodes) {
    /* /dev/zero reads zeros */
    int fd = vfs_open("/dev/zero", O_RDONLY);
    KASSERT_TEST(fd >= 0);
    char b[16]; memset(b, 0xFF, sizeof(b));
    KEXPECT_EQ(vfs_fd_read(fd, b, 16), 16);
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (b[i] != 0) all_zero = 0;
    KEXPECT(all_zero);
    vfs_close(fd);

    /* /dev/null discards writes and reads EOF */
    fd = vfs_open("/dev/null", O_RDWR);
    KASSERT_TEST(fd >= 0);
    KEXPECT_EQ(vfs_fd_write(fd, "ignored", 7), 7);
    KEXPECT_EQ(vfs_fd_read(fd, b, 16), 0);
    vfs_close(fd);

    /* /dev/urandom is not all zeros */
    fd = vfs_open("/dev/urandom", O_RDONLY);
    KASSERT_TEST(fd >= 0);
    memset(b, 0, sizeof(b));
    KEXPECT_EQ(vfs_fd_read(fd, b, 16), 16);
    int any = 0;
    for (int i = 0; i < 16; i++) if (b[i] != 0) any = 1;
    KEXPECT(any);
    vfs_close(fd);
}

KTEST(vfs, initrd_files_loaded) {
    /* Files unpacked from initrd.tar by the bootloader-provided module. */
    int fd = vfs_open("/etc/motd", O_RDONLY);
    KASSERT_TEST(fd >= 0);
    char b[64]; memset(b, 0, sizeof(b));
    long n = vfs_fd_read(fd, b, sizeof(b) - 1);
    KEXPECT(n > 0);
    KEXPECT(strstr(b, "MakhOS") != (void*)0);
    vfs_close(fd);

    KEXPECT(vfs_resolve("/hello.txt") != (void*)0);
}

KTEST(vfs, fd_table_limits) {
    /* Opening distinct fds and closing frees them again. */
    int a = vfs_open("/dev/zero", O_RDONLY);
    int b = vfs_open("/dev/zero", O_RDONLY);
    KEXPECT(a >= 0 && b >= 0 && a != b);
    KEXPECT_EQ(vfs_close(a), 0);
    KEXPECT_EQ(vfs_close(b), 0);
    KEXPECT(vfs_close(a) < 0);      /* already closed */
    KEXPECT(vfs_fd_read(999, (void*)0, 0) < 0);  /* bad fd */
}

/* -------------------------------------------------------------------------- */
/* The open/read/write/close/lseek syscalls route to the VFS (in-kernel path) */
/* -------------------------------------------------------------------------- */

#include <syscall.h>

KTEST(vfs, syscalls_route_to_filesystem) {
    int64_t fd = syscall_handler(SYS_OPEN, (uint64_t)"/sc.txt", O_CREAT | O_RDWR, 0);
    KASSERT_TEST(fd >= 3);                       /* 0..2 reserved for console */
    KEXPECT_EQ(syscall_handler(SYS_WRITE, (uint64_t)fd, (uint64_t)"abcdef", 6), 6);
    KEXPECT_EQ(syscall_handler(SYS_LSEEK, (uint64_t)fd, 0, 0 /*SEEK_SET*/), 0);
    char b[8]; memset(b, 0, sizeof(b));
    KEXPECT_EQ(syscall_handler(SYS_READ, (uint64_t)fd, (uint64_t)b, 6), 6);
    KEXPECT_EQ(memcmp(b, "abcdef", 6), 0);
    KEXPECT_EQ(syscall_handler(SYS_CLOSE, (uint64_t)fd, 0, 0), 0);

    /* Reading the initrd through the syscall layer. */
    fd = syscall_handler(SYS_OPEN, (uint64_t)"/etc/motd", O_RDONLY, 0);
    KASSERT_TEST(fd >= 3);
    memset(b, 0, sizeof(b));
    KEXPECT(syscall_handler(SYS_READ, (uint64_t)fd, (uint64_t)b, 6) == 6);
    syscall_handler(SYS_CLOSE, (uint64_t)fd, 0, 0);
}
