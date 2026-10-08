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

/* Probe the PCI bus for every legacy virtio-blk device and bring each up
 * (negotiate, set up one virtqueue). Returns 0 if at least one came up, -1 if
 * none. Disks are numbered 0..virtio_blk_count()-1. */
int  virtio_blk_init(void);

/* Number of disks brought up, and whether any is present. */
int  virtio_blk_count(void);
int  virtio_blk_present(void);

/* Capacity of `unit` in 512-byte sectors (0 if no such unit). */
uint64_t virtio_blk_capacity(int unit);

/* Read/write `count` sectors starting at `sector` on `unit` into/from `buf`.
 * `buf` must be a physically-contiguous, identity-mapped kernel buffer (e.g. a
 * pmm page) of at least count*512 bytes. Polled (no IRQ). Returns 0, -errno. */
int  virtio_blk_read (int unit, uint64_t sector, void* buf, uint32_t count);
int  virtio_blk_write(int unit, uint64_t sector, const void* buf, uint32_t count);

#endif /* MAKHOS_VIRTIO_BLK_H */
