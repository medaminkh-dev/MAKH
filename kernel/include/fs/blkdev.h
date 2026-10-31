/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - fs/blkdev.h
 * A 512-byte-sector block-device interface, decoupling filesystems from the
 * virtio-blk driver. ext2 sits behind this so it can be driven by the real
 * disk (virtio backend) or by an in-RAM image (ramdisk backend, used by the
 * KFUZZ ext2-image target and, later, by guest disks under the hypervisor).
 *
 * read/write take an LBA in 512-byte sectors and a sector count; they return
 * 0 on success or -errno. `ctx` is the backend's private handle; `nsectors`
 * is the device capacity (a backend rejects any access beyond it).
 */
#ifndef MAKHOS_FS_BLKDEV_H
#define MAKHOS_FS_BLKDEV_H

#include <types.h>

typedef struct blkdev {
    void*    ctx;
    uint64_t nsectors;
    int (*read )(void* ctx, uint64_t lba, void* buf, uint32_t count);
    int (*write)(void* ctx, uint64_t lba, const void* buf, uint32_t count);
} blkdev_t;

#endif /* MAKHOS_FS_BLKDEV_H */
