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
    VNODE_FIFO,         /* pipe (Phase 20-K) */
    VNODE_LNK,          /* symbolic link (U1-a) */
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
    int  (*rename)(struct vnode* olddir, const char* oldname,
                   struct vnode* newdir, const char* newname);     /* same fs */
    /* Create a symlink `name` in `dir` pointing at `target`; returns the new
     * vnode or NULL. Read a symlink's target into `buf` (NUL-terminated),
     * returning its length or -1. (U1-a) */
    struct vnode* (*symlink)(struct vnode* dir, const char* name, const char* target);
    int  (*readlink)(struct vnode* vn, char* buf, size_t sz);
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
    int       refcount;    /* fds sharing this open-file description (dup/fork) */
} file_t;

/* open() flags (subset of the Linux values). */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_CREAT    0x0040
#define O_TRUNC    0x0200
#define O_APPEND   0x0400

/* open() more flags (accepted; mostly advisory here). */
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000
#define O_NONBLOCK  0x0800

/* lseek() whence. */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

#define O_ACCMODE 3        /* mask for the access mode in open flags */

/* fcntl() commands (subset). */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

/* st_mode type bits + S_IS* helpers (POSIX values). */
#define S_IFMT    0170000
#define S_IFCHR   0020000
#define S_IFDIR   0040000
#define S_IFREG   0100000
#define S_IFLNK   0120000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* getdents64 d_type values. */
#define DT_UNKNOWN 0
#define DT_CHR     2
#define DT_DIR     4
#define DT_REG     8

/* struct stat — Linux x86-64 layout, byte-for-byte, so a future musl sees the
 * fields at the offsets it expects. timespecs are embedded as sec/nsec pairs. */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec,  st_atime_nsec;
    int64_t  st_mtime_sec,  st_mtime_nsec;
    int64_t  st_ctime_sec,  st_ctime_nsec;
    int64_t  __unused[3];
};

/* struct linux_dirent64 — the getdents64 record (variable-length name). */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

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

vnode_t*  vfs_resolve(const char* path);          /* follows a trailing symlink */
/* Like vfs_resolve but a trailing symlink is returned as the link itself, not
 * followed — for readlink / lstat (U1-a). */
vnode_t*  vfs_resolve_nofollow(const char* path);
/* Resolve the parent directory of `path` and copy the final component into
 * `leaf` (>= VFS_NAME_MAX+1). Returns the parent vnode or NULL. */
vnode_t*  vfs_resolve_parent(const char* path, char* leaf);

/* Create a symlink at `path` pointing at `target`; 0 or -errno (U1-a). */
int       vfs_symlink(const char* target, const char* path);
/* Read the target of the symlink at `path` into `buf`; length or -errno. */
long      vfs_readlink(const char* path, char* buf, size_t sz);

/* -------- vnode-level helpers -------- */
long      vfs_read(vnode_t* vn, void* buf, size_t n, uint64_t off);
long      vfs_write(vnode_t* vn, const void* buf, size_t n, uint64_t off);
vnode_t*  vfs_create(const char* path, vtype_t type);   /* create file/dir at path */
int       vfs_mkdir(const char* path);
int       vfs_unlink(const char* path);
int       vfs_rename(const char* oldpath, const char* newpath);

/* -------- descriptor layer (uses the current process's table) -------- */
int       vfs_open(const char* path, int flags);
long      vfs_fd_read(int fd, void* buf, size_t n);
long      vfs_fd_write(int fd, const void* buf, size_t n);
long      vfs_lseek(int fd, long off, int whence);
int       vfs_close(int fd);
file_t*   vfs_file(int fd);               /* raw file for a fd, or NULL */

/* -------- pipes / descriptor duplication (Phase 20-K) -------- */
int       vfs_pipe(int fds[2]);           /* fds[0]=read end, fds[1]=write end */
int       vfs_dup2(int oldfd, int newfd); /* share oldfd's description at newfd */
/* Duplicate `src`'s whole fd table into `dst` (fork): shared descriptions,
 * refcounts bumped. Both are process_t*. */
void      vfs_fork_fds(void* dst, void* src);
/* clone(CLONE_FILES): share src's fd table with dst under a refcount. */
void      vfs_share_fds(void* dst, void* src);
/* Drop every open fd of a process (exit): releases each description. */
void      vfs_close_all(void* proc);

/* -------- metadata / listing / fcntl (Phase 20-J) -------- */
int       vfs_stat(const char* path, struct stat* st);
int       vfs_lstat(const char* path, struct stat* st);   /* no-follow (lstat) */
int       vfs_fstat(int fd, struct stat* st);
long      vfs_getdents(int fd, void* buf, size_t n);   /* bytes, 0 at end */
long      vfs_fcntl(int fd, int cmd, long arg);

/* -------- filesystem constructors -------- */
vnode_t*  tmpfs_create_root(void);        /* fs/tmpfs.c */
vnode_t*  tmpfs_link(vnode_t* dir, const char* name, vnode_t* child);
void      devfs_mount(const char* dir);   /* fs/devfs.c: populate a dir with devices */
int       tar_load_initrd(const void* data, size_t len);   /* fs/tar.c */
int       ext2_mount_any(const char* path);  /* fs/ext2.c: mount an ext2 virtio-blk disk */

#endif /* MAKHOS_FS_VFS_H */
