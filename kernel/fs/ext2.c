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
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t align4(uint32_t x) { return (x + 3) & ~3u; }

typedef struct ext2_fs {
    int      unit;                   /* virtio-blk unit */
    uint32_t block_size;             /* bytes per fs block */
    uint32_t spb;                    /* sectors per fs block (block_size/512) */
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t gdt_block;              /* first block of the group-descriptor table */
} ext2_fs_t;

typedef struct ext2_vinfo {
    ext2_fs_t* fs;
    uint32_t   ino;
    uint32_t   size;                 /* file size in bytes */
    uint16_t   mode;                 /* i_mode (type + perms) */
    uint16_t   links;                /* i_links_count */
    uint32_t   blocks512;            /* i_blocks (512-byte units) */
    uint32_t   i_block[15];          /* the inode's block map */
} ext2_vinfo_t;

#define EXT2_S_IFMT  0xF000
#define EXT2_S_IFDIR 0x4000

static struct vfs_ops ext2_ops;      /* defined after the ops below */

/* Read one fs block into `dst` (dst must hold block_size bytes; a pmm page). */
static int bread(ext2_fs_t* fs, uint32_t blk, void* dst) {
    return virtio_blk_read(fs->unit, (uint64_t)blk * fs->spb, dst, fs->spb);
}
/* Write one fs block from `src` (block_size bytes; a pmm page). */
static int bwrite(ext2_fs_t* fs, uint32_t blk, const void* src) {
    return virtio_blk_write(fs->unit, (uint64_t)blk * fs->spb, src, fs->spb);
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
    vi->links = rd16(in + 26);                                    /* i_links_count */
    vi->blocks512 = rd32(in + 28);                                /* i_blocks */
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

/* ---------------------------------------------------------------------------
 * Write path (Phase 20-Q / G2-c). Scoped to block group 0, direct + single-
 * indirect blocks, and regular files. All allocation flips the on-disk bitmap
 * (so the same block/inode is never handed out twice) and keeps the group and
 * superblock free counts honest; the inode's i_block map is zeroed on create,
 * so a freed-then-reused inode never resurrects stale block pointers.
 * ------------------------------------------------------------------------- */

/* Write inode `vi` back into its on-disk slot (read-modify-write the table
 * block so neighbouring inodes are preserved). */
static int write_inode(ext2_fs_t* fs, ext2_vinfo_t* vi) {
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -ENOMEM;
    uint32_t group = (vi->ino - 1) / fs->inodes_per_group;
    uint32_t idx   = (vi->ino - 1) % fs->inodes_per_group;
    uint32_t gd_byte = group * 32;
    if (bread(fs, fs->gdt_block + gd_byte / fs->block_size, blk) != 0) { pmm_free_page(blk); return -EIO; }
    uint32_t itable = rd32(blk + gd_byte % fs->block_size + 8);
    uint32_t in_byte = idx * fs->inode_size;
    uint32_t iblock = itable + in_byte / fs->block_size;
    uint32_t ioff   = in_byte % fs->block_size;
    if (bread(fs, iblock, blk) != 0) { pmm_free_page(blk); return -EIO; }
    uint8_t* in = blk + ioff;
    wr16(in + 0,  vi->mode);
    wr32(in + 4,  vi->size);
    wr16(in + 26, vi->links);
    wr32(in + 28, vi->blocks512);
    for (int i = 0; i < 15; i++) wr32(in + 40 + i * 4, vi->i_block[i]);
    int rc = bwrite(fs, iblock, blk);
    pmm_free_page(blk);
    return rc;
}

/* Flip the first free bit in a group-0 bitmap block, honouring `limit` bits.
 * Returns the bit index, or -1 if the bitmap is full. Writes the bitmap back. */
static long bitmap_alloc(ext2_fs_t* fs, uint32_t bmp_block, uint32_t limit) {
    uint8_t* bm = pmm_alloc_page();
    if (!bm) return -1;
    if (bread(fs, bmp_block, bm) != 0) { pmm_free_page(bm); return -1; }
    long found = -1;
    for (uint32_t i = 0; i < limit; i++) {
        if (!(bm[i / 8] & (1u << (i % 8)))) {
            bm[i / 8] |= (1u << (i % 8));
            found = (long)i;
            break;
        }
    }
    if (found >= 0 && bwrite(fs, bmp_block, bm) != 0) found = -1;
    pmm_free_page(bm);
    return found;
}

/* Decrement a 16-bit field in group-0's descriptor and a 32-bit field in the
 * superblock (the two free-count mirrors), keeping the fs self-consistent. */
static void dec_free_counts(ext2_fs_t* fs, uint32_t gd_off16, uint32_t sb_off32) {
    uint8_t* b = pmm_alloc_page();
    if (!b) return;
    if (bread(fs, fs->gdt_block, b) == 0) {
        uint16_t v = rd16(b + gd_off16); if (v) wr16(b + gd_off16, v - 1);
        bwrite(fs, fs->gdt_block, b);
    }
    if (virtio_blk_read(fs->unit, 2, b, 2) == 0) {       /* superblock @ byte 1024 */
        uint32_t v = rd32(b + sb_off32); if (v) wr32(b + sb_off32, v - 1);
        virtio_blk_write(fs->unit, 2, b, 2);
    }
    pmm_free_page(b);
}

/* Allocate a zeroed data/metadata block (group 0). Returns its block number. */
static uint32_t alloc_block(ext2_fs_t* fs) {
    uint8_t* gd = pmm_alloc_page();
    if (!gd) return 0;
    if (bread(fs, fs->gdt_block, gd) != 0) { pmm_free_page(gd); return 0; }
    uint32_t bbmp = rd32(gd + 0);                        /* bg_block_bitmap */
    pmm_free_page(gd);

    uint32_t limit = fs->blocks_per_group;
    if (limit > fs->block_size * 8) limit = fs->block_size * 8;
    long bit = bitmap_alloc(fs, bbmp, limit);
    if (bit < 0) return 0;
    uint32_t bn = fs->first_data_block + (uint32_t)bit;  /* group 0 */
    if (bn >= fs->blocks_count) return 0;

    uint8_t* z = pmm_alloc_page();                       /* zero the new block */
    if (z) { memset(z, 0, fs->block_size); bwrite(fs, bn, z); pmm_free_page(z); }
    dec_free_counts(fs, 12, 12);                         /* bg/sb free-blocks */
    return bn;
}

/* Allocate a free inode (group 0). Returns its inode number, or 0. */
static uint32_t alloc_inode(ext2_fs_t* fs) {
    uint8_t* gd = pmm_alloc_page();
    if (!gd) return 0;
    if (bread(fs, fs->gdt_block, gd) != 0) { pmm_free_page(gd); return 0; }
    uint32_t ibmp = rd32(gd + 4);                        /* bg_inode_bitmap */
    pmm_free_page(gd);

    uint32_t limit = fs->inodes_per_group;
    if (limit > fs->block_size * 8) limit = fs->block_size * 8;
    long bit = bitmap_alloc(fs, ibmp, limit);
    if (bit < 0) return 0;
    uint32_t ino = (uint32_t)bit + 1;                    /* inodes are 1-based */
    if (ino > fs->inodes_count) return 0;
    dec_free_counts(fs, 14, 16);                         /* bg/sb free-inodes */
    return ino;
}

/* Increment a 16-bit group-descriptor field and a 32-bit superblock field —
 * the inverse of dec_free_counts, used when freeing a block or inode. */
static void inc_free_counts(ext2_fs_t* fs, uint32_t gd_off16, uint32_t sb_off32) {
    uint8_t* b = pmm_alloc_page();
    if (!b) return;
    if (bread(fs, fs->gdt_block, b) == 0) {
        wr16(b + gd_off16, (uint16_t)(rd16(b + gd_off16) + 1));
        bwrite(fs, fs->gdt_block, b);
    }
    if (virtio_blk_read(fs->unit, 2, b, 2) == 0) {
        wr32(b + sb_off32, rd32(b + sb_off32) + 1);
        virtio_blk_write(fs->unit, 2, b, 2);
    }
    pmm_free_page(b);
}

/* Clear a bit in a group-0 bitmap block. */
static void bitmap_free(ext2_fs_t* fs, uint32_t bmp_block, uint32_t bit) {
    uint8_t* bm = pmm_alloc_page();
    if (!bm) return;
    if (bread(fs, bmp_block, bm) == 0) {
        bm[bit / 8] &= ~(1u << (bit % 8));
        bwrite(fs, bmp_block, bm);
    }
    pmm_free_page(bm);
}

static void free_block(ext2_fs_t* fs, uint32_t bn) {
    if (bn < fs->first_data_block) return;
    uint8_t* gd = pmm_alloc_page();
    if (!gd) return;
    if (bread(fs, fs->gdt_block, gd) != 0) { pmm_free_page(gd); return; }
    uint32_t bbmp = rd32(gd + 0);
    pmm_free_page(gd);
    bitmap_free(fs, bbmp, bn - fs->first_data_block);    /* group 0 */
    inc_free_counts(fs, 12, 12);
}

static void free_inode_num(ext2_fs_t* fs, uint32_t ino) {
    if (ino == 0) return;
    uint8_t* gd = pmm_alloc_page();
    if (!gd) return;
    if (bread(fs, fs->gdt_block, gd) != 0) { pmm_free_page(gd); return; }
    uint32_t ibmp = rd32(gd + 4);
    pmm_free_page(gd);
    bitmap_free(fs, ibmp, ino - 1);                      /* group 0 */
    inc_free_counts(fs, 14, 16);
}

/* Free every data block of `vi` with file-relative index >= `from`, plus the
 * single-indirect block itself when nothing past the direct blocks remains. */
static void free_from(ext2_fs_t* fs, ext2_vinfo_t* vi, uint32_t from) {
    uint32_t nblocks = (vi->size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = from; b < nblocks; b++) {
        uint32_t disk = map_block(fs, vi, b);
        if (disk) { free_block(fs, disk); vi->blocks512 -= fs->spb; }
        if (b < EXT2_NDIR) vi->i_block[b] = 0;
    }
    uint32_t per = fs->block_size / 4;
    if (from <= EXT2_NDIR && vi->i_block[EXT2_IND]) {    /* indirect no longer needed */
        /* zero the pointers we freed above inside the indirect block is moot —
         * we are releasing the whole indirect block. */
        free_block(fs, vi->i_block[EXT2_IND]);
        vi->blocks512 -= fs->spb;
        vi->i_block[EXT2_IND] = 0;
    } else if (vi->i_block[EXT2_IND] && from > EXT2_NDIR && from < EXT2_NDIR + per) {
        uint8_t* ind = pmm_alloc_page();                 /* clear freed slots */
        if (ind && bread(fs, vi->i_block[EXT2_IND], ind) == 0) {
            for (uint32_t i = from - EXT2_NDIR; i < per; i++) wr32(ind + i * 4, 0);
            bwrite(fs, vi->i_block[EXT2_IND], ind);
        }
        if (ind) pmm_free_page(ind);
    }
}

/* Allocate the file-relative block `fblk` for inode `vi`, wiring it into the
 * block map (direct or single-indirect). Returns the disk block, or 0. */
static uint32_t alloc_file_block(ext2_fs_t* fs, ext2_vinfo_t* vi, uint32_t fblk) {
    if (fblk < EXT2_NDIR) {
        uint32_t b = alloc_block(fs); if (!b) return 0;
        vi->i_block[fblk] = b; vi->blocks512 += fs->spb; return b;
    }
    uint32_t per = fs->block_size / 4;
    if (fblk < EXT2_NDIR + per) {
        if (!vi->i_block[EXT2_IND]) {                    /* need the indirect block */
            uint32_t ib = alloc_block(fs); if (!ib) return 0;
            vi->i_block[EXT2_IND] = ib; vi->blocks512 += fs->spb;
        }
        uint32_t b = alloc_block(fs); if (!b) return 0;
        vi->blocks512 += fs->spb;
        uint8_t* ind = pmm_alloc_page(); if (!ind) return 0;
        int ok = (bread(fs, vi->i_block[EXT2_IND], ind) == 0);
        if (ok) { wr32(ind + (fblk - EXT2_NDIR) * 4, b); ok = (bwrite(fs, vi->i_block[EXT2_IND], ind) == 0); }
        pmm_free_page(ind);
        return ok ? b : 0;
    }
    return 0;                                            /* double-indirect deferred */
}

static long ext2_write(vnode_t* vn, const void* buf, size_t n, uint64_t off) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)vn->priv;
    if (!vi) return -EINVAL;
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
            disk = alloc_file_block(fs, vi, fblk);
            if (disk == 0) { pmm_free_page(blk); write_inode(fs, vi);
                             return done ? (long)done : -ENOSPC; }
        }
        if (chunk < fs->block_size) {                    /* partial: read-modify-write */
            if (bread(fs, disk, blk) != 0) { pmm_free_page(blk); return done ? (long)done : -EIO; }
        } else {
            memset(blk, 0, fs->block_size);
        }
        memcpy(blk + boff, (const uint8_t*)buf + done, chunk);
        if (bwrite(fs, disk, blk) != 0) { pmm_free_page(blk); return done ? (long)done : -EIO; }
        done += chunk;
    }
    pmm_free_page(blk);
    if (off + n > vi->size) { vi->size = (uint32_t)(off + n); vn->size = vi->size; }
    write_inode(fs, vi);
    return (long)done;
}

/* Add a directory entry {ino,ftype,name} to `dir`, splitting an existing
 * entry's slack or, failing that, growing the directory by a block. */
static int dir_add_entry(vnode_t* dir, const char* name, uint32_t ino, uint8_t ftype) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)dir->priv;
    ext2_fs_t* fs = vi->fs;
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t need = align4(8 + nlen);
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -ENOMEM;

    uint32_t nblocks = (vi->size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t disk = map_block(fs, vi, b);
        if (!disk || bread(fs, disk, blk) != 0) continue;
        uint32_t o = 0;
        while (o + 8 <= fs->block_size) {
            uint32_t e_ino = rd32(blk + o);
            uint16_t rlen  = rd16(blk + o + 4);
            uint8_t  e_nl  = blk[o + 6];
            if (rlen < 8 || o + rlen > fs->block_size) break;
            uint32_t ideal = (e_ino == 0) ? 0 : align4(8 + e_nl);
            if (rlen - ideal >= need) {
                uint32_t no; uint16_t nrl;
                if (e_ino == 0) { no = o; nrl = rlen; }
                else { wr16(blk + o + 4, (uint16_t)ideal); no = o + ideal; nrl = (uint16_t)(rlen - ideal); }
                wr32(blk + no, ino); wr16(blk + no + 4, nrl);
                blk[no + 6] = (uint8_t)nlen; blk[no + 7] = ftype;
                memcpy(blk + no + 8, name, nlen);
                int rc = bwrite(fs, disk, blk);
                pmm_free_page(blk);
                return rc;
            }
            o += rlen;
        }
    }
    /* No slack anywhere: grow the directory by one block holding this entry. */
    uint32_t nb = alloc_file_block(fs, vi, nblocks);
    if (!nb) { pmm_free_page(blk); return -ENOSPC; }
    memset(blk, 0, fs->block_size);
    wr32(blk + 0, ino); wr16(blk + 4, (uint16_t)fs->block_size);
    blk[6] = (uint8_t)nlen; blk[7] = ftype;
    memcpy(blk + 8, name, nlen);
    int rc = bwrite(fs, nb, blk);
    pmm_free_page(blk);
    if (rc != 0) return rc;
    vi->size += fs->block_size; dir->size = vi->size;
    return write_inode(fs, vi);
}

/* Remove the entry `name` from `dir` and return its inode in *out_ino, WITHOUT
 * touching the target's link count (that is the caller's job: unlink frees,
 * rename re-homes). Merges the slot into the previous entry, or voids it. */
static int dir_remove_entry(vnode_t* dir, const char* name, uint32_t* out_ino) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)dir->priv;
    ext2_fs_t* fs = vi->fs;
    uint32_t nlen = (uint32_t)strlen(name);
    uint8_t* blk = pmm_alloc_page();
    if (!blk) return -ENOMEM;
    uint32_t nblocks = (vi->size + fs->block_size - 1) / fs->block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t disk = map_block(fs, vi, b);
        if (!disk || bread(fs, disk, blk) != 0) continue;
        uint32_t o = 0, prev = 0; int have_prev = 0;
        while (o + 8 <= fs->block_size) {
            uint32_t e_ino = rd32(blk + o);
            uint16_t rlen  = rd16(blk + o + 4);
            uint8_t  e_nl  = blk[o + 6];
            if (rlen < 8 || o + rlen > fs->block_size) break;
            if (e_ino != 0 && e_nl == nlen && memcmp(blk + o + 8, name, nlen) == 0) {
                if (out_ino) *out_ino = e_ino;
                if (have_prev) wr16(blk + prev + 4, (uint16_t)(rd16(blk + prev + 4) + rlen));
                else           wr32(blk + o, 0);          /* first in block: void it */
                int rc = bwrite(fs, disk, blk);
                pmm_free_page(blk);
                return rc;
            }
            prev = o; have_prev = 1; o += rlen;
        }
    }
    pmm_free_page(blk);
    return -ENOENT;
}

/* True if `dir` holds only "." and "..". */
static int dir_is_empty(vnode_t* dir) {
    char nm[256]; uint32_t ino; uint8_t ft; uint32_t idx = 0, real = 0;
    while (dir_walk(dir, NULL, idx, &ino, &ft, nm) == 0) {
        if (!(nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))) real++;
        idx++;
    }
    return real == 0;
}

static vnode_t* ext2_create(vnode_t* dir, const char* name, vtype_t t) {
    ext2_vinfo_t* dvi = (ext2_vinfo_t*)dir->priv;
    if (!dvi || dir->type != VNODE_DIR) return NULL;
    if (t != VNODE_REG && t != VNODE_DIR) return NULL;
    ext2_fs_t* fs = dvi->fs;

    uint32_t ino = alloc_inode(fs);
    if (!ino) return NULL;

    if (t == VNODE_DIR) {
        /* A new directory: one data block holding "." and "..". */
        uint32_t dblk = alloc_block(fs);
        if (!dblk) { free_inode_num(fs, ino); return NULL; }
        uint8_t* blk = pmm_alloc_page();
        if (!blk) { free_block(fs, dblk); free_inode_num(fs, ino); return NULL; }
        memset(blk, 0, fs->block_size);
        wr32(blk + 0, ino);  wr16(blk + 4, 12); blk[6] = 1; blk[7] = 2; blk[8] = '.';
        wr32(blk + 12, dvi->ino); wr16(blk + 16, (uint16_t)(fs->block_size - 12));
        blk[18] = 2; blk[19] = 2; blk[20] = '.'; blk[21] = '.';
        int wr = bwrite(fs, dblk, blk);
        pmm_free_page(blk);
        if (wr != 0) { free_block(fs, dblk); free_inode_num(fs, ino); return NULL; }

        ext2_vinfo_t nw;
        memset(&nw, 0, sizeof(nw));
        nw.fs = fs; nw.ino = ino; nw.mode = 0x41ED /* dir, 0755 */;
        nw.size = fs->block_size; nw.links = 2; nw.blocks512 = fs->spb;
        nw.i_block[0] = dblk;
        if (write_inode(fs, &nw) != 0) return NULL;

        if (dir_add_entry(dir, name, ino, 2 /* EXT2_FT_DIR */) != 0) return NULL;
        dvi->links++;                                    /* the new dir's ".." */
        write_inode(fs, dvi);
        return make_vnode(fs, ino);
    }

    ext2_vinfo_t nw;                                     /* fresh regular file */
    memset(&nw, 0, sizeof(nw));
    nw.fs = fs; nw.ino = ino; nw.mode = 0x81A4 /* 0644 */; nw.links = 1;
    if (write_inode(fs, &nw) != 0) return NULL;
    if (dir_add_entry(dir, name, ino, 1 /* EXT2_FT_REG_FILE */) != 0) return NULL;
    return make_vnode(fs, ino);
}

static int ext2_truncate(vnode_t* vn, uint64_t len) {
    ext2_vinfo_t* vi = (ext2_vinfo_t*)vn->priv;
    if (!vi) return -EINVAL;
    ext2_fs_t* fs = vi->fs;
    if (len < vi->size) {
        uint32_t keep = (uint32_t)((len + fs->block_size - 1) / fs->block_size);
        free_from(fs, vi, keep);
    }
    vi->size = (uint32_t)len; vn->size = len;            /* growth is lazy (write allocs) */
    return write_inode(fs, vi);
}

static int ext2_unlink(vnode_t* dir, const char* name) {
    ext2_vinfo_t* dvi = (ext2_vinfo_t*)dir->priv;
    if (!dvi || dir->type != VNODE_DIR) return -EINVAL;
    ext2_fs_t* fs = dvi->fs;

    /* Find the child first so we can handle a directory (rmdir) vs a file. */
    uint32_t ino = 0; uint8_t ft = 0;
    if (dir_walk(dir, name, 0, &ino, &ft, NULL) != 0) return -ENOENT;
    ext2_vinfo_t cvi;
    if (read_inode(fs, ino, &cvi) != 0) return -EIO;

    if ((cvi.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {      /* rmdir path */
        vnode_t* cv = make_vnode(fs, ino);
        if (!cv) return -EIO;
        int empty = dir_is_empty(cv);
        kfree(cv->priv); kfree(cv);
        if (!empty) return -ENOTEMPTY;
    }

    uint32_t removed = 0;
    int rc = dir_remove_entry(dir, name, &removed);
    if (rc != 0) return rc;

    if ((cvi.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        free_from(fs, &cvi, 0);                          /* the dir's single block */
        free_inode_num(fs, ino);
        if (dvi->links > 0) dvi->links--;                /* the child's ".." is gone */
        write_inode(fs, dvi);
    } else {
        if (cvi.links > 0) cvi.links--;
        if (cvi.links == 0) { free_from(fs, &cvi, 0); free_inode_num(fs, ino); }
        else                 write_inode(fs, &cvi);
    }
    return 0;
}

/* rename within the same ext2 mount: re-home `oldname` in olddir to `newname`
 * in newdir, replacing an existing target. A moved directory's ".." and the two
 * parents' link counts are fixed up for a cross-directory move. */
static int ext2_rename(vnode_t* olddir, const char* oldname,
                       vnode_t* newdir, const char* newname) {
    ext2_vinfo_t* ovi = (ext2_vinfo_t*)olddir->priv;
    ext2_vinfo_t* nvi = (ext2_vinfo_t*)newdir->priv;
    if (!ovi || !nvi || ovi->fs != nvi->fs) return -EINVAL;
    ext2_fs_t* fs = ovi->fs;

    uint32_t ino = 0; uint8_t ft = 0;
    if (dir_walk(olddir, oldname, 0, &ino, &ft, NULL) != 0) return -ENOENT;
    ext2_vinfo_t cvi;
    if (read_inode(fs, ino, &cvi) != 0) return -EIO;
    int is_dir = (cvi.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    uint32_t existing = 0;                                /* replace a target if present */
    if (dir_walk(newdir, newname, 0, &existing, &ft, NULL) == 0) {
        if (ext2_unlink(newdir, newname) != 0) return -EEXIST;
    }

    if (dir_add_entry(newdir, newname, ino, is_dir ? 2 : 1) != 0) return -ENOSPC;
    if (dir_remove_entry(olddir, oldname, NULL) != 0) return -EIO;

    if (is_dir && ovi->ino != nvi->ino) {                /* cross-dir: fix ".." + links */
        uint8_t* blk = pmm_alloc_page();
        if (blk && bread(fs, cvi.i_block[0], blk) == 0) {
            /* entry 1 is ".." (after "." at offset 0, rec_len 12) */
            wr32(blk + 12, nvi->ino);
            bwrite(fs, cvi.i_block[0], blk);
        }
        if (blk) pmm_free_page(blk);
        if (ovi->links > 0) ovi->links--;
        write_inode(fs, ovi);
        nvi->links++;
        write_inode(fs, nvi);
    }
    return 0;
}

static vfs_ops_t ext2_ops = {
    .read = ext2_read,
    .write = ext2_write,
    .lookup = ext2_lookup,
    .create = ext2_create,
    .readdir = ext2_readdir,
    .unlink = ext2_unlink,
    .truncate = ext2_truncate,
    .rename = ext2_rename,
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
    fs->blocks_count     = rd32(sb + 4);
    fs->inodes_count     = rd32(sb + 0);
    fs->blocks_per_group = rd32(sb + 32);
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
