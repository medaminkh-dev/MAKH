/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - drivers/virtio_blk.h
 * A polled legacy virtio-blk (virtio-pci 0.9.5) block device. Phase 20-P (G2-a):
 * the first persistent-storage brick — a disk MAKH can read and write in units
 * of 512-byte sectors, the substrate a real on-disk filesystem (ext2) sits on.
 */
#ifndef MAKHOS_VIRTIO_BLK_H
#define MAKHOS_VIRTIO_BLK_H

#include <types.h>

#define VIRTIO_BLK_SECTOR 512

/* Probe the PCI bus for a legacy virtio-blk device and bring it up (negotiate,
 * set up one virtqueue). Returns 0 on success, -1 if none / init failed. */
int  virtio_blk_init(void);

/* Is a device present and initialised? */
int  virtio_blk_present(void);

/* Capacity in 512-byte sectors. */
uint64_t virtio_blk_capacity(void);

/* Read/write `count` sectors starting at `sector` into/from `buf`. `buf` must be
 * a physically-contiguous, identity-mapped kernel buffer (e.g. a pmm page) of at
 * least count*512 bytes. Polled (no IRQ). Returns 0 on success, -errno. */
int  virtio_blk_read (uint64_t sector, void* buf, uint32_t count);
int  virtio_blk_write(uint64_t sector, const void* buf, uint32_t count);

#endif /* MAKHOS_VIRTIO_BLK_H */
