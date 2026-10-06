/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/devfs.c
 * Character devices exposed as vnodes (Phase 18): /dev/null, /dev/zero,
 * /dev/console, /dev/urandom. They are plain VNODE_CHR vnodes linked into a
 * tmpfs directory, each with its own read/write behaviour.
 */

#include <fs/vfs.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <vga.h>
#include <errno.h>

enum { DEV_NULL, DEV_ZERO, DEV_CONSOLE, DEV_URANDOM };

static long dev_read(vnode_t* vn, void* buf, size_t n, uint64_t off) {
    (void)off;
    switch ((long)(uintptr_t)vn->priv) {
        case DEV_ZERO:
            memset(buf, 0, n);
            return (long)n;
        case DEV_URANDOM: {
            /* xorshift64*; a real CSPRNG comes later, this is for entropy fill. */
            static uint64_t s = 0x1234567689ABCDEFull;
            uint8_t* p = buf;
            for (size_t i = 0; i < n; i++) {
                s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                p[i] = (uint8_t)(s * 0x2545F4914F6CDD1Dull >> 56);
            }
            return (long)n;
        }
        case DEV_NULL:
        case DEV_CONSOLE:
        default:
            return 0;                       /* EOF */
    }
}

static long dev_write(vnode_t* vn, const void* buf, size_t n, uint64_t off) {
    (void)off;
    switch ((long)(uintptr_t)vn->priv) {
        case DEV_CONSOLE: {
            const char* p = buf;
            for (size_t i = 0; i < n; i++) terminal_putchar(p[i]);
            return (long)n;
        }
        case DEV_NULL:
        case DEV_ZERO:
            return (long)n;                 /* accepted and discarded */
        default:
            return -EINVAL;
    }
}

static const vfs_ops_t dev_ops = { .read = dev_read, .write = dev_write };

static vnode_t* mk_dev(int which) {
    vnode_t* vn = kcalloc(1, sizeof(vnode_t));
    if (!vn) return NULL;
    vn->type = VNODE_CHR;
    vn->ops = &dev_ops;
    vn->priv = (void*)(uintptr_t)which;
    return vn;
}

void devfs_mount(const char* dirpath) {
    vfs_mkdir(dirpath);
    vnode_t* dir = vfs_resolve(dirpath);
    if (!dir) return;
    tmpfs_link(dir, "null",    mk_dev(DEV_NULL));
    tmpfs_link(dir, "zero",    mk_dev(DEV_ZERO));
    tmpfs_link(dir, "console", mk_dev(DEV_CONSOLE));
    tmpfs_link(dir, "urandom", mk_dev(DEV_URANDOM));
}
