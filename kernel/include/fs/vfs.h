/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/vfs.h
 * Virtual filesystem layer (Phase 18).
 *
 * One small object graph sits between programs and the filesystems:
 *
 *   fd (per process)  ->  struct file (offset, flags)  ->  vnode  ->  vfs_ops
 *
 * A vnode is a filesystem-agnostic handle to a file, directory or device; the
 * filesystem (tmpfs, devfs, later ext2) supplies the vfs_ops and the private
 * data behind it. A directory vnode may have another filesystem's root mounted
 * on it; path resolution crosses the mount transparently.
 */

#ifndef MAKHOS_FS_VFS_H
#define MAKHOS_FS_VFS_H

#include <types.h>

#define VFS_NAME_MAX   63
#define VFS_PATH_MAX   256
#define VFS_MAX_FDS    32

typedef enum vtype {
    VNODE_NONE = 0,
    VNODE_REG,          /* regular file     */
    VNODE_DIR,          /* directory        */
    VNODE_CHR,          /* character device */
} vtype_t;

struct vnode;

/* A filesystem implements these. NULL means "not supported". Offsets are byte
 * offsets; returns are byte counts or -errno. */
typedef struct vfs_ops {
    long (*read)(struct vnode* vn, void* buf, size_t n, uint64_t off);
    long (*write)(struct vnode* vn, const void* buf, size_t n, uint64_t off);
    struct vnode* (*lookup)(struct vnode* dir, const char* name);          /* child or NULL */
    struct vnode* (*create)(struct vnode* dir, const char* name, vtype_t t);/* new child    */
    int  (*readdir)(struct vnode* dir, uint32_t index, char* name_out);     /* 0 / -1 at end */
    int  (*unlink)(struct vnode* dir, const char* name);
    int  (*truncate)(struct vnode* vn, uint64_t len);
} vfs_ops_t;

typedef struct vnode {
    vtype_t             type;
    const vfs_ops_t*    ops;
    void*               priv;      /* filesystem-private */
    uint64_t            size;
    uint32_t            refcount;
    struct vnode*       mounted;   /* if a filesystem is mounted here, its root */
} vnode_t;

typedef struct file {
    vnode_t*  vnode;
    uint64_t  offset;
    int       flags;
    int       used;
} file_t;

/* open() flags (subset of the Linux values). */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* lseek() whence. */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* -------- lifecycle -------- */
void      vfs_init(void);                 /* mount a tmpfs as "/" */
int       vfs_mount(const char* path, vnode_t* root);
vnode_t*  vfs_root(void);

/* -------- path resolution -------- */
/* Canonicalise (cwd, path) into an absolute path in out[outsz]: an absolute
 * `path` ignores cwd; a relative one is taken from cwd. Resolves ".", ".." and
 * runs of '/'; the result is absolute, has no "." / ".." / "//", and never
 * climbs above "/". Returns 0, -EINVAL on bad args, or -ERANGE if the result
 * (or its component count) would overflow. Pure string work — no filesystem. */
int       path_canonicalize(const char* cwd, const char* path,
                            char* out, size_t outsz);

vnode_t*  vfs_resolve(const char* path);
/* Resolve the parent directory of `path` and copy the final component into
 * `leaf` (>= VFS_NAME_MAX+1). Returns the parent vnode or NULL. */
vnode_t*  vfs_resolve_parent(const char* path, char* leaf);

/* -------- vnode-level helpers -------- */
long      vfs_read(vnode_t* vn, void* buf, size_t n, uint64_t off);
long      vfs_write(vnode_t* vn, const void* buf, size_t n, uint64_t off);
vnode_t*  vfs_create(const char* path, vtype_t type);   /* create file/dir at path */
int       vfs_mkdir(const char* path);
int       vfs_unlink(const char* path);

/* -------- descriptor layer (uses the current process's table) -------- */
int       vfs_open(const char* path, int flags);
long      vfs_fd_read(int fd, void* buf, size_t n);
long      vfs_fd_write(int fd, const void* buf, size_t n);
long      vfs_lseek(int fd, long off, int whence);
int       vfs_close(int fd);
file_t*   vfs_file(int fd);               /* raw file for a fd, or NULL */

/* -------- filesystem constructors -------- */
vnode_t*  tmpfs_create_root(void);        /* fs/tmpfs.c */
vnode_t*  tmpfs_link(vnode_t* dir, const char* name, vnode_t* child);
void      devfs_mount(const char* dir);   /* fs/devfs.c: populate a dir with devices */
int       tar_load_initrd(const void* data, size_t len);   /* fs/tar.c */

#endif /* MAKHOS_FS_VFS_H */
