/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/tar.c
 * Load a USTAR archive (the initrd) into the root tmpfs (Phase 18).
 *
 * A tar is a sequence of 512-byte headers, each followed by the file content
 * padded to 512 bytes. We create parent directories as needed and write each
 * regular file's bytes into a fresh tmpfs file. The parser is strict about
 * bounds (the archive is untrusted input the fuzzer also feeds), so a
 * truncated or malformed header stops the load instead of running off the end.
 */

#include <fs/vfs.h>
#include <lib/string.h>
#include <klog.h>

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed)) tar_header_t;

/* Parse an octal ASCII field of at most `len` chars. */
static uint64_t oct(const char* s, int len) {
    uint64_t v = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        v = (v << 3) | (uint64_t)(s[i] - '0');
    return v;
}

/* Ensure every directory along `path` (excluding the final component) exists. */
static void make_parents(const char* path) {
    char buf[VFS_PATH_MAX];
    int n = 0;
    for (int i = 0; path[i] && n < VFS_PATH_MAX - 1; i++) {
        if (path[i] == '/' && n > 0) {
            buf[n] = '\0';
            vfs_mkdir(buf);        /* harmless if it already exists */
        }
        buf[n++] = path[i];
    }
}

int tar_load_initrd(const void* data, size_t len) {
    const uint8_t* base = data;
    size_t off = 0;
    int files = 0;

    while (off + 512 <= len) {
        const tar_header_t* h = (const tar_header_t*)(base + off);
        if (h->name[0] == '\0') break;             /* end-of-archive padding */
        if (memcmp(h->magic, "ustar", 5) != 0) break;   /* not a USTAR header */

        uint64_t fsize = oct(h->size, sizeof(h->size));
        off += 512;
        if (off + fsize > len) break;              /* truncated payload */

        /* Build an absolute path from the (relative) tar name. */
        char path[VFS_PATH_MAX];
        int n = 0;
        path[n++] = '/';
        for (int i = 0; h->name[i] && i < 100 && n < VFS_PATH_MAX - 1; i++) path[n++] = h->name[i];
        path[n] = '\0';

        if (h->typeflag == '5') {                  /* directory */
            if (n > 1 && path[n - 1] == '/') path[n - 1] = '\0';
            make_parents(path);
            vfs_mkdir(path);
        } else if (h->typeflag == '0' || h->typeflag == '\0') {   /* regular file */
            make_parents(path);
            vnode_t* vn = vfs_create(path, VNODE_REG);
            if (vn && fsize) vfs_write(vn, base + off, (size_t)fsize, 0);
            files++;
        }

        off += (fsize + 511) & ~(uint64_t)511;     /* advance past padded content */
    }

    KLOG_I("INITRD", "loaded %d file(s) from a %lu-byte archive\n",
           files, (unsigned long)len);
    return files;
}
