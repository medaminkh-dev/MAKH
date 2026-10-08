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
#include <sched.h>
#include <irq.h>
#include <signal.h>

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

int vfs_rename(const char* oldpath, const char* newpath) {
    char oleaf[VFS_NAME_MAX + 1], nleaf[VFS_NAME_MAX + 1];
    vnode_t* od = vfs_resolve_parent(oldpath, oleaf);
    vnode_t* nd = vfs_resolve_parent(newpath, nleaf);
    if (!od || !nd || oleaf[0] == '\0' || nleaf[0] == '\0') return -EINVAL;
    /* Same filesystem only (same ops table); cross-fs rename is a copy+unlink
     * the caller must do itself. */
    if (!od->ops || !od->ops->rename || od->ops != nd->ops) return -EINVAL;
    return od->ops->rename(od, oleaf, nd, nleaf);
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
    f->refcount = 1;              /* one fd owns this description */
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

static void pipe_detach(vnode_t* vn, int access);   /* fwd (Phase 20-K) */

/* Release one reference to an open-file description. The caller has already
 * removed it from its fd slot. At the last reference the backing is released:
 * a pipe end is detached (waking the far side) and its vnode freed when no end
 * remains; a regular vnode just has its open-count decremented. */
static void file_put(file_t* f) {
    if (!f) return;
    if (--f->refcount > 0) return;
    vnode_t* vn = f->vnode;
    if (vn) {
        if (vn->type == VNODE_FIFO) pipe_detach(vn, f->flags & O_ACCMODE);
        if (vn->refcount) vn->refcount--;
        if (vn->type == VNODE_FIFO && vn->refcount == 0) {
            kfree(vn->priv);
            kfree(vn);
        }
    }
    kfree(f);
}

int vfs_close(int fd) {
    file_t** t = fd_table();
    if (!t || fd < 0 || fd >= VFS_MAX_FDS || !t[fd]) return -EBADF;
    file_t* f = t[fd];
    t[fd] = NULL;
    file_put(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Metadata / listing / fcntl (Phase 20-J)                                    */
/* -------------------------------------------------------------------------- */

static void fill_stat(vnode_t* vn, struct stat* st) {
    memset(st, 0, sizeof(*st));
    uint32_t mode;
    switch (vn->type) {
        case VNODE_DIR: mode = S_IFDIR | 0755; break;
        case VNODE_CHR: mode = S_IFCHR | 0666; break;
        default:        mode = S_IFREG | 0644; break;
    }
    st->st_mode    = mode;
    st->st_nlink   = 1;
    st->st_size    = (int64_t)vn->size;
    st->st_blksize = 4096;
    st->st_blocks  = (int64_t)((vn->size + 511) / 512);
    st->st_ino     = (uint64_t)(uintptr_t)vn;   /* stable per-vnode identity */
}

int vfs_stat(const char* path, struct stat* st) {
    vnode_t* vn = vfs_resolve(path);
    if (!vn) return -ENOENT;
    fill_stat(vn, st);
    return 0;
}

int vfs_fstat(int fd, struct stat* st) {
    if (fd >= 0 && fd <= 2) {                   /* the console/tty: a char device */
        memset(st, 0, sizeof(*st));
        st->st_mode  = S_IFCHR | 0620;
        st->st_nlink = 1;
        st->st_rdev  = 0x0501;                  /* arbitrary (major 5, minor 1) */
        return 0;
    }
    file_t* f = vfs_file(fd);
    if (!f || !f->vnode) return -EBADF;
    fill_stat(f->vnode, st);
    return 0;
}

/* getdents64 into a kernel buffer; returns bytes written (0 at end of dir). */
long vfs_getdents(int fd, void* buf, size_t n) {
    file_t* f = vfs_file(fd);
    if (!f || !f->vnode) return -EBADF;
    vnode_t* dir = f->vnode;
    if (dir->type != VNODE_DIR || !dir->ops || !dir->ops->readdir) return -ENOTDIR;

    const size_t hdr = __builtin_offsetof(struct linux_dirent64, d_name);
    uint8_t* out = (uint8_t*)buf;
    size_t used = 0;
    char name[VFS_NAME_MAX + 1];

    for (;;) {
        uint32_t idx = (uint32_t)f->offset;
        if (dir->ops->readdir(dir, idx, name) != 0) break;       /* end of dir */
        size_t namelen = strlen(name);
        size_t reclen = (hdr + namelen + 1 + 7) & ~(size_t)7;    /* 8-aligned */
        if (used + reclen > n) {
            if (used == 0) return -EINVAL;      /* buffer can't hold one entry */
            break;                               /* resume here next call */
        }
        struct linux_dirent64* d = (struct linux_dirent64*)(out + used);
        d->d_ino    = (uint64_t)idx + 1;
        d->d_off    = (int64_t)idx + 1;
        d->d_reclen = (uint16_t)reclen;
        d->d_type   = DT_UNKNOWN;
        if (dir->ops->lookup) {
            vnode_t* c = dir->ops->lookup(dir, name);
            if (c) d->d_type = (c->type == VNODE_DIR) ? DT_DIR
                             : (c->type == VNODE_CHR) ? DT_CHR : DT_REG;
        }
        memcpy(d->d_name, name, namelen);
        for (size_t p = hdr + namelen; p < reclen; p++) out[used + p] = 0; /* pad+NUL */
        used += reclen;
        f->offset++;
    }
    return (long)used;
}

long vfs_fcntl(int fd, int cmd, long arg) {
    file_t* f = vfs_file(fd);
    if (!f && (fd < 0 || fd > 2)) return -EBADF;   /* 0/1/2 have no file_t */
    switch (cmd) {
        case F_GETFL: return f ? f->flags : O_RDWR;
        case F_SETFL:
            if (f) f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK))
                            | ((int)arg & (O_APPEND | O_NONBLOCK));
            return 0;
        case F_GETFD: return 0;                    /* no FD_CLOEXEC tracking yet */
        case F_SETFD: return 0;                    /* accept, ignore */
        case F_DUPFD: {                            /* lowest free fd >= arg */
            file_t** t = fd_table();
            if (!t || !f) return -EBADF;
            int lo = (int)arg; if (lo < 0) lo = 0;
            for (int i = lo; i < VFS_MAX_FDS; i++) {
                if (!t[i]) { t[i] = f; f->refcount++; return i; }
            }
            return -EMFILE;
        }
        default:      return -EINVAL;
    }
}

/* -------------------------------------------------------------------------- */
/* Pipes + descriptor duplication (Phase 20-K)                                */
/* -------------------------------------------------------------------------- */

#define PIPE_CAP 4096

typedef struct pipe {
    uint8_t  buf[PIPE_CAP];
    size_t   head, tail, count;
    int      readers, writers;   /* live read-end / write-end descriptions */
    wait_queue_t rwait, wwait;   /* readers wait for data; writers for space */
} pipe_t;

static long pipe_read(vnode_t* vn, void* dst, size_t n, uint64_t off) {
    (void)off;
    pipe_t* p = (pipe_t*)vn->priv;
    uint8_t* out = (uint8_t*)dst;
    size_t got = 0;
    irqflags_t f = local_irq_save();
    while (got < n) {
        if (p->count == 0) {
            if (p->writers == 0) break;                  /* EOF: no writers */
            if (got > 0) break;                          /* return partial */
            int rc = sched_wait_event(&p->rwait, 0, f);  /* restores f */
            if (rc == 2) return got ? (long)got : -EINTR;
            f = local_irq_save();
            continue;
        }
        size_t chunk = n - got;
        if (chunk > p->count) chunk = p->count;
        for (size_t i = 0; i < chunk; i++) {
            out[got + i] = p->buf[p->tail];
            p->tail = (p->tail + 1) % PIPE_CAP;
        }
        p->count -= chunk;
        got += chunk;
        wq_wake_all(&p->wwait);                           /* space freed */
        break;                                            /* one burst */
    }
    local_irq_restore(f);
    return (long)got;
}

static long pipe_write(vnode_t* vn, const void* src, size_t n, uint64_t off) {
    (void)off;
    pipe_t* p = (pipe_t*)vn->priv;
    const uint8_t* in = (const uint8_t*)src;
    size_t put = 0;
    irqflags_t f = local_irq_save();
    while (put < n) {
        if (p->readers == 0) {                            /* broken pipe */
            local_irq_restore(f);
            if (current_process) signal_send(current_process, SIGPIPE);
            return put ? (long)put : -EPIPE;
        }
        if (p->count == PIPE_CAP) {                       /* full: wait */
            int rc = sched_wait_event(&p->wwait, 0, f);
            if (rc == 2) return put ? (long)put : -EINTR;
            f = local_irq_save();
            continue;
        }
        size_t space = PIPE_CAP - p->count;
        size_t chunk = n - put;
        if (chunk > space) chunk = space;
        for (size_t i = 0; i < chunk; i++) {
            p->buf[p->head] = in[put + i];
            p->head = (p->head + 1) % PIPE_CAP;
        }
        p->count += chunk;
        put += chunk;
        wq_wake_all(&p->rwait);                           /* data ready */
    }
    local_irq_restore(f);
    return (long)put;
}

static const vfs_ops_t pipe_ops = { .read = pipe_read, .write = pipe_write };

static void pipe_detach(vnode_t* vn, int access) {
    pipe_t* p = (pipe_t*)vn->priv;
    if (!p) return;
    irqflags_t f = local_irq_save();
    if (access == O_WRONLY) { if (p->writers) p->writers--; }
    else                    { if (p->readers) p->readers--; }
    local_irq_restore(f);
    wq_wake_all(&p->rwait);     /* last writer gone -> readers see EOF   */
    wq_wake_all(&p->wwait);     /* last reader gone -> writers see EPIPE */
}

int vfs_pipe(int fds[2]) {
    file_t** t = fd_table();
    if (!t) return -ENOMEM;

    vnode_t* vn = kcalloc(1, sizeof(vnode_t));
    pipe_t*  p  = kcalloc(1, sizeof(pipe_t));
    file_t*  rf = kcalloc(1, sizeof(file_t));
    file_t*  wf = kcalloc(1, sizeof(file_t));
    if (!vn || !p || !rf || !wf) { kfree(vn); kfree(p); kfree(rf); kfree(wf); return -ENOMEM; }

    wq_init(&p->rwait);
    wq_init(&p->wwait);
    p->readers = 1; p->writers = 1;
    vn->type = VNODE_FIFO;
    vn->ops  = &pipe_ops;
    vn->priv = p;
    vn->refcount = 2;                     /* the read-end + write-end file_t's */

    rf->vnode = vn; rf->flags = O_RDONLY; rf->used = 1; rf->refcount = 1;
    wf->vnode = vn; wf->flags = O_WRONLY; wf->used = 1; wf->refcount = 1;

    int rfd = fd_alloc(t);
    if (rfd < 0) { kfree(vn); kfree(p); kfree(rf); kfree(wf); return rfd; }
    t[rfd] = rf;
    int wfd = fd_alloc(t);
    if (wfd < 0) { t[rfd] = NULL; kfree(vn); kfree(p); kfree(rf); kfree(wf); return wfd; }
    t[wfd] = wf;

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

int vfs_dup2(int oldfd, int newfd) {
    file_t** t = fd_table();
    if (!t) return -ENOMEM;
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || !t[oldfd]) return -EBADF;
    if (newfd < 0 || newfd >= VFS_MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (t[newfd]) { file_t* old = t[newfd]; t[newfd] = NULL; file_put(old); }
    t[newfd] = t[oldfd];
    t[newfd]->refcount++;                 /* another fd on this description */
    return newfd;
}

/* fork: the child shares every open description (refcounts bumped). */
void vfs_fork_fds(void* dstp, void* srcp) {
    process_t* d = (process_t*)dstp;
    process_t* s = (process_t*)srcp;
    file_t** st = (file_t**)s->fd_table;
    if (!st) return;                       /* parent had none (lazy table) */
    file_t** dt = kcalloc(VFS_MAX_FDS, sizeof(file_t*));
    if (!dt) return;
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (st[i]) { dt[i] = st[i]; st[i]->refcount++; }
    d->fd_table = dt;
}

/* clone(CLONE_FILES): the child shares the parent's fd table by pointer, under a
 * refcount so neither frees it until the last sharer exits (Phase 20-L). */
void vfs_share_fds(void* dstp, void* srcp) {
    process_t* d = (process_t*)dstp;
    process_t* s = (process_t*)srcp;
    if (!s->fd_table) {
        s->fd_table = kcalloc(VFS_MAX_FDS, sizeof(file_t*));
        if (!s->fd_table) return;
    }
    if (!s->fd_rc) {
        s->fd_rc = (int*)kmalloc(sizeof(int));
        if (!s->fd_rc) return;             /* fall back: child just gets none */
        *s->fd_rc = 1;
    }
    (*s->fd_rc)++;
    d->fd_table = s->fd_table;
    d->fd_rc    = s->fd_rc;
}

/* exit: drop every fd so pipe peers see EOF/EPIPE and descriptions free. For a
 * shared table (clone CLONE_FILES) only the last thread closes and frees it. */
void vfs_close_all(void* procp) {
    process_t* p = (process_t*)procp;
    file_t** t = (file_t**)p->fd_table;
    if (!t) return;
    if (p->fd_rc) {
        int left = --(*p->fd_rc);
        p->fd_table = NULL;
        if (left > 0) { p->fd_rc = NULL; return; }   /* siblings still use it */
        kfree(p->fd_rc);
        p->fd_rc = NULL;
    } else {
        p->fd_table = NULL;
    }
    for (int i = 0; i < VFS_MAX_FDS; i++)
        if (t[i]) { file_t* f = t[i]; t[i] = NULL; file_put(f); }
    kfree(t);
}
