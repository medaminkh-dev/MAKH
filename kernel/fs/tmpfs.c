/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/tmpfs.c
 * A RAM-backed filesystem (Phase 18). Directories are singly linked lists of
 * named children; regular files are a heap buffer that grows on write. It is
 * the root filesystem and the substrate devfs and the initrd populate.
 */

#include <fs/vfs.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <errno.h>

typedef struct tmpfs_dirent {
    char                 name[VFS_NAME_MAX + 1];
    vnode_t*             vnode;
    struct tmpfs_dirent* next;
} tmpfs_dirent_t;

/* priv for a directory: its child list. priv for a regular file: a buffer. */
typedef struct tmpfs_dir  { tmpfs_dirent_t* head; } tmpfs_dir_t;
typedef struct tmpfs_file { uint8_t* data; uint64_t cap; } tmpfs_file_t;

static const vfs_ops_t tmpfs_ops;   /* forward */

static vnode_t* mk_vnode(vtype_t type) {
    vnode_t* vn = kcalloc(1, sizeof(vnode_t));
    if (!vn) return NULL;
    vn->type = type;
    vn->ops = &tmpfs_ops;
    if (type == VNODE_DIR) {
        vn->priv = kcalloc(1, sizeof(tmpfs_dir_t));
    } else if (type == VNODE_REG || type == VNODE_LNK) {
        vn->priv = kcalloc(1, sizeof(tmpfs_file_t));   /* a symlink stores its target here */
    }
    if (type != VNODE_CHR && !vn->priv) { kfree(vn); return NULL; }
    return vn;
}

static vnode_t* tmpfs_lookup(vnode_t* dir, const char* name) {
    if (dir->type != VNODE_DIR) return NULL;
    for (tmpfs_dirent_t* e = ((tmpfs_dir_t*)dir->priv)->head; e; e = e->next)
        if (strcmp(e->name, name) == 0) return e->vnode;
    return NULL;
}

/* Insert an already-built vnode as a named child (used by devfs/initrd too). */
vnode_t* tmpfs_link(vnode_t* dir, const char* name, vnode_t* child) {
    if (!dir || dir->type != VNODE_DIR || !child) return NULL;
    tmpfs_dirent_t* e = kcalloc(1, sizeof(tmpfs_dirent_t));
    if (!e) return NULL;
    int i = 0;
    for (; name[i] && i < VFS_NAME_MAX; i++) e->name[i] = name[i];
    e->name[i] = '\0';
    e->vnode = child;
    tmpfs_dir_t* d = (tmpfs_dir_t*)dir->priv;
    e->next = d->head;
    d->head = e;
    child->refcount++;
    return child;
}

static vnode_t* tmpfs_create(vnode_t* dir, const char* name, vtype_t type) {
    if (tmpfs_lookup(dir, name)) return NULL;
    vnode_t* vn = mk_vnode(type);
    if (!vn) return NULL;
    if (!tmpfs_link(dir, name, vn)) { kfree(vn->priv); kfree(vn); return NULL; }
    return vn;
}

static int tmpfs_readdir(vnode_t* dir, uint32_t index, char* name_out) {
    if (dir->type != VNODE_DIR) return -1;
    for (tmpfs_dirent_t* e = ((tmpfs_dir_t*)dir->priv)->head; e; e = e->next) {
        if (index == 0) {
            int i = 0; for (; e->name[i]; i++) name_out[i] = e->name[i]; name_out[i] = '\0';
            return 0;
        }
        index--;
    }
    return -1;
}

static long tmpfs_read(vnode_t* vn, void* buf, size_t n, uint64_t off) {
    if (vn->type != VNODE_REG) return -EINVAL;
    if (off >= vn->size) return 0;
    uint64_t avail = vn->size - off;
    if (n > avail) n = (size_t)avail;
    memcpy(buf, ((tmpfs_file_t*)vn->priv)->data + off, n);
    return (long)n;
}

static int tmpfs_truncate(vnode_t* vn, uint64_t len);

static long tmpfs_write(vnode_t* vn, const void* buf, size_t n, uint64_t off) {
    if (vn->type != VNODE_REG) return -EINVAL;
    tmpfs_file_t* f = (tmpfs_file_t*)vn->priv;
    uint64_t end = off + n;
    if (end > f->cap) {
        uint64_t ncap = f->cap ? f->cap : 64;
        while (ncap < end) ncap *= 2;
        uint8_t* nd = krealloc(f->data, (size_t)ncap);
        if (!nd) return -ENOMEM;
        f->data = nd;
        f->cap = ncap;
    }
    if (off > vn->size) memset(f->data + vn->size, 0, (size_t)(off - vn->size));  /* sparse fill */
    memcpy(f->data + off, buf, n);
    if (end > vn->size) vn->size = end;
    return (long)n;
}

static int tmpfs_truncate(vnode_t* vn, uint64_t len) {
    if (vn->type != VNODE_REG) return -EINVAL;
    vn->size = len;
    return 0;
}

/* Symlinks (U1-a): the target string is kept in the same buffer a regular file
 * uses, so it reclaims through the VNODE_REG/VNODE_LNK path in free_vnode. */
static vnode_t* tmpfs_symlink(vnode_t* dir, const char* name, const char* target) {
    if (tmpfs_lookup(dir, name)) return NULL;
    vnode_t* vn = mk_vnode(VNODE_LNK);
    if (!vn) return NULL;
    size_t len = strlen(target);
    tmpfs_file_t* f = (tmpfs_file_t*)vn->priv;
    f->data = kmalloc(len + 1);
    if (!f->data) { kfree(vn->priv); kfree(vn); return NULL; }
    memcpy(f->data, target, len + 1);
    f->cap = len + 1;
    vn->size = len;
    if (!tmpfs_link(dir, name, vn)) { kfree(f->data); kfree(vn->priv); kfree(vn); return NULL; }
    return vn;
}

static int tmpfs_readlink(vnode_t* vn, char* buf, size_t sz) {
    if (vn->type != VNODE_LNK || !vn->priv || sz == 0) return -1;
    tmpfs_file_t* f = (tmpfs_file_t*)vn->priv;
    if (!f->data) return -1;
    size_t len = vn->size;
    if (len >= sz) len = sz - 1;            /* truncate to fit, keep NUL-terminated */
    memcpy(buf, f->data, len);
    buf[len] = '\0';
    return (int)len;
}

static void free_vnode(vnode_t* vn) {
    if (!vn) return;
    if ((vn->type == VNODE_REG || vn->type == VNODE_LNK) && vn->priv) {
        kfree(((tmpfs_file_t*)vn->priv)->data);
        kfree(vn->priv);
    } else if (vn->type == VNODE_DIR && vn->priv) {
        kfree(vn->priv);   /* caller must have emptied the child list */
    }
    kfree(vn);
}

static int tmpfs_unlink(vnode_t* dir, const char* name) {
    if (dir->type != VNODE_DIR) return -ENOTDIR;
    tmpfs_dir_t* d = (tmpfs_dir_t*)dir->priv;
    tmpfs_dirent_t** pp = &d->head;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            tmpfs_dirent_t* e = *pp;
            vnode_t* vn = e->vnode;
            if (vn->type == VNODE_DIR && ((tmpfs_dir_t*)vn->priv)->head) return -ENOTEMPTY;
            *pp = e->next;
            kfree(e);
            if (vn->refcount) vn->refcount--;
            if (vn->refcount == 0) free_vnode(vn);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -ENOENT;
}

/* Move the entry `oldname` in `olddir` to `newname` in `newdir` (same fs). The
 * dirent node is reused and re-threaded, so the child's refcount is unchanged;
 * an existing, empty destination is removed first. */
static int tmpfs_rename(vnode_t* olddir, const char* oldname,
                        vnode_t* newdir, const char* newname) {
    if (olddir->type != VNODE_DIR || newdir->type != VNODE_DIR) return -ENOTDIR;
    tmpfs_dir_t* od = (tmpfs_dir_t*)olddir->priv;

    tmpfs_dirent_t** pp = &od->head;
    while (*pp && strcmp((*pp)->name, oldname) != 0) pp = &(*pp)->next;
    if (!*pp) return -ENOENT;
    tmpfs_dirent_t* src = *pp;

    vnode_t* dstvn = tmpfs_lookup(newdir, newname);
    if (dstvn) {
        if (dstvn == src->vnode) return 0;                     /* same file */
        if (dstvn->type == VNODE_DIR && ((tmpfs_dir_t*)dstvn->priv)->head)
            return -ENOTEMPTY;
        int rc = tmpfs_unlink(newdir, newname);
        if (rc < 0) return rc;
        pp = &od->head;                                        /* list may have shifted */
        while (*pp && *pp != src) pp = &(*pp)->next;
        if (!*pp) return -ENOENT;
    }

    *pp = src->next;                                           /* detach from olddir */
    int i = 0;
    for (; newname[i] && i < VFS_NAME_MAX; i++) src->name[i] = newname[i];
    src->name[i] = '\0';
    tmpfs_dir_t* nd = (tmpfs_dir_t*)newdir->priv;              /* attach to newdir */
    src->next = nd->head;
    nd->head = src;
    return 0;
}

static const vfs_ops_t tmpfs_ops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .readdir = tmpfs_readdir,
    .unlink = tmpfs_unlink,
    .truncate = tmpfs_truncate,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
    .rename = tmpfs_rename,
};

vnode_t* tmpfs_create_root(void) {
    vnode_t* root = mk_vnode(VNODE_DIR);
    if (root) root->refcount = 1;
    return root;
}
