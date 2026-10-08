/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_virtio_blk.c
 * Phase 20-P (G2-a): the first persistent-storage brick. A legacy virtio-blk
 * disk (QEMU, snapshot=on) is read and written in 512-byte sectors:
 *   - sector 0 carries a known magic + sentinel + byte ramp (tools/mkdisk.py)
 *     that the read path must reproduce exactly;
 *   - a write to a scratch sector, read back, must round-trip (writes are
 *     ephemeral under snapshot=on, so parallel stress runs stay isolated).
 */

#include <ktest.h>
#include <drivers/virtio_blk.h>
#include <mm/pmm.h>
#include <lib/string.h>

KTEST(vblk, reads_known_sector0) {
    KASSERT_TEST(virtio_blk_present());
    KEXPECT_EQ((int)virtio_blk_capacity(0), 2048);     /* unit 0: the 1 MiB raw image */

    uint8_t* buf = pmm_alloc_page();
    KASSERT_TEST(buf != 0);
    memset(buf, 0, 512);

    KEXPECT_EQ(virtio_blk_read(0, 0, buf, 1), 0);      /* unit 0, sector 0 */
    KEXPECT_EQ(memcmp(buf, "MAKHDSK1", 8), 0);         /* magic */
    uint32_t sentinel = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8)
                      | ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    KEXPECT_EQ((int)sentinel, (int)0xC0FFEE42);        /* 32-bit LE sentinel */
    KEXPECT_EQ((int)buf[100], 100);                    /* ramp byte i == i&0xFF */

    pmm_free_page(buf);
}

KTEST(vblk, write_read_roundtrip) {
    KASSERT_TEST(virtio_blk_present());
    uint8_t* w = pmm_alloc_page();
    uint8_t* r = pmm_alloc_page();
    KASSERT_TEST(w != 0 && r != 0);

    for (int i = 0; i < 512; i++) w[i] = (uint8_t)(0xA5 ^ (i * 7));
    memset(r, 0, 512);

    KEXPECT_EQ(virtio_blk_write(0, 100, w, 1), 0);     /* unit 0; ephemeral (snapshot=on) */
    KEXPECT_EQ(virtio_blk_read(0, 100, r, 1), 0);
    KEXPECT_EQ(memcmp(w, r, 512), 0);                  /* same bytes back */

    pmm_free_page(w);
    pmm_free_page(r);
}
