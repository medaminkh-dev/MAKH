/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/ext2.c
 * A read-only ext2 filesystem over a virtio-blk disk (Phase 20-Q / G2-b).
 *
 * Just enough of ext2 to mount a real on-disk image into the VFS and read
 * files from it: parse the superblock and the block-group descriptors, fetch
 * inodes, follow a file's block map (direct + single-indirect), walk directory
 * entries, and expose it all through vfs_ops so programs open on-disk files the
 * same way they open tmpfs ones. Writing is deferred — this is the read path.
 *
 * All disk I/O goes through a PMM scratch page (identity-mapped, the DMA buffer
 * virtio-blk requires); file data is then memcpy'd into the caller's buffer, so
 * callers may pass any kernel pointer.
 */

#include <fs/vfs.h>
#include <drivers/virtio_blk.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <errno.h>
#include <klog.h>

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INO    2
#define EXT2_NDIR        12          /* direct block pointers */
#define EXT2_IND         12          /* index of the single-indirect pointer */

/* Little-endian field reads from a raw buffer (x86 is LE, so just load). */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct ext2_fs {
    int      unit;                   /* virtio-blk unit */
    uint32_t block_size;             /* bytes per fs block */
    uint32_t spb;                    /* sectors per fs block (block_size/512) */
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t gdt_block;              /* first block of the group-descriptor table */
} ext2_fs_t;

typedef struct ext2_vinfo {
    ext2_fs_t* fs;
    uint32_t   ino;
    uint32_t   size;                 /* file size in bytes */
    uint16_t   mode;                 /* i_mode (type + perms) */
    uint32_t   i_block[15];          /* the inode's block map */
} ext2_vinfo_t;

#define EXT2_S_IFMT  0xF000
#define EXT2_S_IFDIR 0x4000

static struct vfs_ops ext2_ops;      /* defined after the ops below */

/* Read one fs block into `dst` (dst must hold block_size bytes; a pmm page). */
static int bread(ext2_fs_t* fs, uint32_t blk, void* dst) {
    return virtio_blk_read(fs->unit, (uint64_t)blk * fs->spb, dst, fs->spb);
}

/* Load inode `ino` into `vi` (block map + size). Returns 0 or -errno. */
static int read_inode(ext2_fs_t* fs, uint32_t ino, ext2_vinfo_t* vi) {
    if (ino == 0) return -EINVAL;
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -ENOMEM;

    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (ino - 1) % fs->inodes_per_group;

    /* Group descriptor for `group`: 32 bytes each, in the GDT. */
    uint32_t gd_byte = group * 32;
    if (bread(fs, fs->gdt_block + gd_byte / fs->block_size, blk) != 0) { pmm_free_page(blk); return -EIO; }
    uint32_t itable = rd32(blk + gd_byte % fs->block_size + 8);   /* bg_inode_table */

    /* The inode itself. */
    uint32_t in_byte = idx * fs->inode_size;
    if (bread(fs, itable + in_byte / fs->block_size, blk) != 0) { pmm_free_page(blk); return -EIO; }
    const uint8_t* in = blk + in_byte % fs->block_size;
    vi->fs = fs;
    vi->ino = ino;
    vi->mode = rd16(in + 0);                                      /* i_mode */
    vi->size = rd32(in + 4);                                      /* i_size (low) */
    for (int i = 0; i < 15; i++) vi->i_block[i] = rd32(in + 40 + i * 4);

    pmm_free_page(blk);
    return 0;
}

/* Map file block index `fblk` to an on-disk block number (0 = sparse/hole).
 * Direct blocks and the single-indirect block are supported. */
static uint32_t map_block(ext2_fs_t* fs, ext2_vinfo_t* vi, uint32_t fblk) {
    if (fblk < EXT2_NDIR) return vi->i_block[fblk];
    uint32_t per = fs->block_size / 4;                            /* pointers per block */
    if (fblk < EXT2_NDIR + per) {
        uint32_t ind = vi->i_block[EXT2_IND];
        if (!ind) return 0;
        uint8_t* blk = pmm_alloc_page();
        if (!blk) return 0;
        uint32_t out = 0;
        if (bread(fs, ind, blk) == 0) out = rd32(blk + (fblk - EXT2_NDIR) * 4);
        pmm_free_page(blk);
        return out;
    }
    return 0;    /* double/triple indirect: deferred */
}

/* Build a vnode for inode `ino`. The type comes from the inode's i_mode, which
 * is authoritative (independent of whether the fs has the dir-entry filetype
 * feature). */
static vnode_t* make_vnode(ext2_fs_t* fs, uint32_t ino) {
    ext2_vinfo_t* vi = kcalloc(1, sizeof(*vi));
    if (!vi) return NULL;
    if (read_inode(fs, ino, vi) != 0) { kfree(vi); return NULL; }
    vnode_t* vn = kcalloc(1, sizeof(*vn));
    if (!vn) { kfree(vi); return NULL; }
    vn->type = ((vi->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? VNODE_DIR : VNODE_REG;
    vn->size = vi->size;
    vn->priv = vi;
    vn->ops  = &ext2_ops;
    return vn;
}

/* Read up to `n` bytes of file `vn` from byte offset `off`. */
static long ext2_read(vnode_t* vn, void* buf, size_t n, uint64_t off) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)vn->priv;
    if (!vi) return -EINVAL;
    if (off >= vi->size) return 0;
    if (off + n > vi->size) n = vi->size - off;

    ext2_fs_t* fs = vi->fs;
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -ENOMEM;

    size_t done = 0;
    while (done < n) {
        uint64_t fpos = off + done;
        uint32_t fblk = (uint32_t)(fpos / fs->block_size);
        uint32_t boff = (uint32_t)(fpos % fs->block_size);
        uint32_t chunk = fs->block_size - boff;
        if (chunk > n - done) chunk = (uint32_t)(n - done);

        uint32_t disk = map_block(fs, vi, fblk);
        if (disk == 0) {
            memset((uint8_t*)buf + done, 0, chunk);              /* sparse hole */
        } else {
            if (bread(fs, disk, blk) != 0) { pmm_free_page(blk); return done ? (long)done : -EIO; }
            memcpy((uint8_t*)buf + done, blk + boff, chunk);
        }
        done += chunk;
    }
    pmm_free_page(blk);
    return (long)done;
}

/* Scan directory `dir`'s data blocks, calling found() per entry. A small helper
 * so lookup and readdir share the walk. `want` is a name to match (lookup) or
 * NULL; `index` selects the nth entry (readdir). */
static int dir_walk(vnode_t* dir, const char* want, uint32_t index,
                    uint32_t* out_ino, uint8_t* out_ftype, char* out_name) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)dir->priv;
    if (!vi || dir->type != VNODE_DIR) return -1;
    ext2_fs_t* fs = vi->fs;
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -1;

    uint32_t nblocks = (vi->size + fs->block_size - 1) / fs->block_size;
    uint32_t seen = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t disk = map_block(fs, vi, b);
        if (!disk || bread(fs, disk, blk) != 0) continue;
        uint32_t o = 0;
        while (o + 8 <= fs->block_size) {
            uint32_t ino   = rd32(blk + o);
            uint16_t rlen  = rd16(blk + o + 4);
            uint8_t  nlen  = blk[o + 6];
            uint8_t  ftype = blk[o + 7];
            if (rlen < 8) break;                                 /* corrupt: stop */
            if (ino != 0 && nlen != 0) {
                const char* name = (const char*)(blk + o + 8);
                if (want) {
                    if (nlen == strlen(want) && memcmp(name, want, nlen) == 0) {
                        *out_ino = ino; *out_ftype = ftype;
                        pmm_free_page(blk); return 0;
                    }
                } else if (seen == index) {
                    uint8_t k = nlen; if (k > 254) k = 254;
                    memcpy(out_name, name, k); out_name[k] = '\0';
                    *out_ino = ino; *out_ftype = ftype;
                    pmm_free_page(blk); return 0;
                }
                seen++;
            }
            o += rlen;
        }
    }
    pmm_free_page(blk);
    return -1;
}

static vnode_t* ext2_lookup(vnode_t* dir, const char* name) {
    uint32_t ino; uint8_t ftype;
    if (dir_walk(dir, name, 0, &ino, &ftype, NULL) != 0) return NULL;
    (void)ftype;                                 /* type comes from the inode */
    ext2_vinfo_t* vi = (ext2_vinfo_t*)dir->priv;
    return make_vnode(vi->fs, ino);
}

static int ext2_readdir(vnode_t* dir, uint32_t index, char* name_out) {
    uint32_t ino; uint8_t ftype;
    if (dir_walk(dir, NULL, index, &ino, &ftype, name_out) != 0) return -1;
    return 0;
}

static struct vfs_ops ext2_ops = {
    .read = ext2_read,
    .write = NULL,
    .lookup = ext2_lookup,
    .create = NULL,
    .readdir = ext2_readdir,
    .unlink = NULL,
    .truncate = NULL,
};

/* Mount the ext2 filesystem on virtio-blk `unit`; returns its root vnode or
 * NULL if `unit` does not hold a valid ext2 image. */
static vnode_t* ext2_mount_unit(int unit) {
    uint8_t* sb = pmm_alloc_page();
    if (!sb) return NULL;
    /* Superblock lives at byte offset 1024 — sector 2, two 512-byte sectors. */
    if (virtio_blk_read(unit, 2, sb, 2) != 0) { pmm_free_page(sb); return NULL; }
    if (rd16(sb + 56) != EXT2_MAGIC) { pmm_free_page(sb); return NULL; }

    ext2_fs_t* fs = kcalloc(1, sizeof(*fs));
    if (!fs) { pmm_free_page(sb); return NULL; }
    fs->unit             = unit;
    fs->block_size       = 1024u << rd32(sb + 24);               /* s_log_block_size */
    fs->inodes_per_group = rd32(sb + 40);
    fs->first_data_block = rd32(sb + 20);
    uint16_t isz         = rd16(sb + 88);                        /* s_inode_size */
    fs->inode_size       = isz ? isz : 128;                      /* rev0 = 128 */
    fs->spb              = fs->block_size / 512;
    fs->gdt_block        = fs->first_data_block + 1;
    pmm_free_page(sb);

    if (fs->block_size > 4096 || fs->spb == 0 || fs->inodes_per_group == 0) {
        KLOG_E("EXT2", "unsupported geometry (bs=%u)\n", fs->block_size);
        kfree(fs); return NULL;
    }

    vnode_t* root = make_vnode(fs, EXT2_ROOT_INO);
    if (!root) { kfree(fs); return NULL; }
    KLOG_I("EXT2", "mounted unit %d: block_size=%u inode_size=%u\n",
           unit, fs->block_size, fs->inode_size);
    return root;
}

/* Find the first virtio-blk disk holding an ext2 image and mount it at `path`.
 * Returns 0 on success, -1 if no ext2 disk was found. */
int ext2_mount_any(const char* path) {
    int n = virtio_blk_count();
    for (int u = 0; u < n; u++) {
        vnode_t* root = ext2_mount_unit(u);
        if (root) {
            if (vfs_mount(path, root) != 0) { KLOG_E("EXT2", "vfs_mount(%s) failed\n", path); return -1; }
            KLOG_I("EXT2", "ext2 on virtio-blk unit %d mounted at %s\n", u, path);
            return 0;
        }
    }
    return -1;
}
