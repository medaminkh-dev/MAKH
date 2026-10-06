/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/vfs.c
 * Virtual filesystem core: path resolution, mounts and the descriptor layer.
 * See fs/vfs.h. Filesystem behaviour lives in tmpfs.c / devfs.c.
 */

#include <fs/vfs.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <errno.h>
#include <klog.h>
#include <proc_internal.h>

static vnode_t* root_vnode;

vnode_t* vfs_root(void) { return root_vnode; }

void vfs_init(void) {
    root_vnode = tmpfs_create_root();
    if (!root_vnode) { KLOG_E("VFS", "could not create root tmpfs\n"); return; }
    KLOG_I("VFS", "root tmpfs mounted at /\n");
}

/* A vnode with a filesystem mounted on it resolves to that filesystem's root. */
static vnode_t* cross_mount(vnode_t* vn) {
    while (vn && vn->mounted) vn = vn->mounted;
    return vn;
}

int vfs_mount(const char* path, vnode_t* mroot) {
    vnode_t* at = vfs_resolve(path);
    if (!at || at->type != VNODE_DIR) return -ENOTDIR;
    at->mounted = mroot;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Path resolution                                                            */
/* -------------------------------------------------------------------------- */

/* Walk `path`; if want_parent, stop at the last component and return its
 * directory, copying the final name into `leaf`. */
static vnode_t* walk(const char* path, int want_parent, char* leaf) {
    if (!path || path[0] != '/') return NULL;      /* absolute paths only */
    vnode_t* cur = cross_mount(root_vnode);
    if (!cur) return NULL;

    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        /* Extract one component. */
        char name[VFS_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/' && n < VFS_NAME_MAX) name[n++] = *p++;
        name[n] = '\0';
        while (*p && *p != '/') p++;                /* skip an over-long tail */

        /* Is this the final component? */
        const char* q = p;
        while (*q == '/') q++;
        int last = (*q == '\0');

        if (last && want_parent) {
            if (leaf) { int i = 0; for (; name[i]; i++) leaf[i] = name[i]; leaf[i] = '\0'; }
            return cur->type == VNODE_DIR ? cur : NULL;
        }

        if (cur->type != VNODE_DIR || !cur->ops || !cur->ops->lookup) return NULL;
        if (name[0] == '.' && name[1] == '\0') continue;     /* "." */
        vnode_t* child = cur->ops->lookup(cur, name);
        if (!child) return NULL;
        cur = cross_mount(child);
    }

    if (want_parent) {                              /* path was "/" */
        if (leaf) leaf[0] = '\0';
        return cur->type == VNODE_DIR ? cur : NULL;
    }
    return cur;
}

vnode_t* vfs_resolve(const char* path) {
    if (path && path[0] == '/' && path[1] == '\0') return cross_mount(root_vnode);
    return walk(path, 0, NULL);
}

vnode_t* vfs_resolve_parent(const char* path, char* leaf) {
    return walk(path, 1, leaf);
}

/* -------------------------------------------------------------------------- */
/* vnode helpers                                                              */
/* -------------------------------------------------------------------------- */

long vfs_read(vnode_t* vn, void* buf, size_t n, uint64_t off) {
    if (!vn || !vn->ops || !vn->ops->read) return -EINVAL;
    return vn->ops->read(vn, buf, n, off);
}

long vfs_write(vnode_t* vn, const void* buf, size_t n, uint64_t off) {
    if (!vn || !vn->ops || !vn->ops->write) return -EINVAL;
    return vn->ops->write(vn, buf, n, off);
}

vnode_t* vfs_create(const char* path, vtype_t type) {
    char leaf[VFS_NAME_MAX + 1];
    vnode_t* dir = vfs_resolve_parent(path, leaf);
    if (!dir || leaf[0] == '\0' || !dir->ops || !dir->ops->create) return NULL;
    vnode_t* existing = dir->ops->lookup ? dir->ops->lookup(dir, leaf) : NULL;
    if (existing) return existing;
    return dir->ops->create(dir, leaf, type);
}

int vfs_mkdir(const char* path) {
    return vfs_create(path, VNODE_DIR) ? 0 : -EEXIST;
}

int vfs_unlink(const char* path) {
    char leaf[VFS_NAME_MAX + 1];
    vnode_t* dir = vfs_resolve_parent(path, leaf);
    if (!dir || leaf[0] == '\0' || !dir->ops || !dir->ops->unlink) return -EINVAL;
    return dir->ops->unlink(dir, leaf);
}

/* -------------------------------------------------------------------------- */
/* Descriptor layer (per-process fd table in process_t.fd_table)              */
/* -------------------------------------------------------------------------- */

static file_t** fd_table(void) {
    /* Lazily attach a table to the current process. */
    if (!current_process) return NULL;
    if (!current_process->fd_table) {
        current_process->fd_table = kcalloc(VFS_MAX_FDS, sizeof(file_t*));
    }
    return (file_t**)current_process->fd_table;
}

file_t* vfs_file(int fd) {
    file_t** t = fd_table();
    if (!t || fd < 0 || fd >= VFS_MAX_FDS) return NULL;
    return t[fd];
}

static int fd_alloc(file_t** t) {
    /* 0,1,2 are reserved for console stdin/stdout/stderr (handled in the
     * syscall layer), so real files start at fd 3. */
    for (int i = 3; i < VFS_MAX_FDS; i++) if (!t[i]) return i;
    return -EMFILE;
}

int vfs_open(const char* path, int flags) {
    file_t** t = fd_table();
    if (!t) return -ENOMEM;

    vnode_t* vn = vfs_resolve(path);
    if (!vn && (flags & O_CREAT)) vn = vfs_create(path, VNODE_REG);
    if (!vn) return -ENOENT;
    if ((flags & O_TRUNC) && vn->ops && vn->ops->truncate) vn->ops->truncate(vn, 0);

    int fd = fd_alloc(t);
    if (fd < 0) return fd;
    file_t* f = kcalloc(1, sizeof(file_t));
    if (!f) return -ENOMEM;
    f->vnode = vn;
    f->flags = flags;
    f->offset = (flags & O_APPEND) ? vn->size : 0;
    f->used = 1;
    vn->refcount++;
    t[fd] = f;
    return fd;
}

long vfs_fd_read(int fd, void* buf, size_t n) {
    file_t* f = vfs_file(fd);
    if (!f) return -EBADF;
    long r = vfs_read(f->vnode, buf, n, f->offset);
    if (r > 0) f->offset += (uint64_t)r;
    return r;
}

long vfs_fd_write(int fd, const void* buf, size_t n) {
    file_t* f = vfs_file(fd);
    if (!f) return -EBADF;
    if (f->flags & O_APPEND) f->offset = f->vnode->size;
    long r = vfs_write(f->vnode, buf, n, f->offset);
    if (r > 0) f->offset += (uint64_t)r;
    return r;
}

long vfs_lseek(int fd, long off, int whence) {
    file_t* f = vfs_file(fd);
    if (!f) return -EBADF;
    uint64_t base = (whence == SEEK_CUR) ? f->offset
                  : (whence == SEEK_END) ? f->vnode->size : 0;
    long target = (long)base + off;
    if (target < 0) return -EINVAL;
    f->offset = (uint64_t)target;
    return target;
}

int vfs_close(int fd) {
    file_t** t = fd_table();
    if (!t || fd < 0 || fd >= VFS_MAX_FDS || !t[fd]) return -EBADF;
    if (t[fd]->vnode && t[fd]->vnode->refcount) t[fd]->vnode->refcount--;
    kfree(t[fd]);
    t[fd] = NULL;
    return 0;
}
