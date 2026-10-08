/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - drivers/virtio_blk.c
 * A minimal, polled legacy virtio-blk driver (virtio-pci 0.9.5 / transitional).
 * See virtio_blk.h.
 *
 * Legacy virtio-pci exposes its control registers through an I/O-port BAR and
 * the device's queue size is fixed (the driver reads it, it does not choose).
 * We bring up exactly one split virtqueue in a physically-contiguous region
 * (its address handed to the device as a page-frame number), and run every
 * request synchronously by polling the used ring — no interrupts. That is all
 * an on-disk filesystem needs to read and write sectors.
 *
 * DMA model: PMM frames are identity-mapped, so a buffer's virtual address is
 * its physical (DMA) address — the same assumption the e1000 driver relies on.
 */

#include <drivers/virtio_blk.h>
#include <drivers/pci.h>
#include <mm/pmm.h>
#include <lib/string.h>
#include <kernel.h>
#include <errno.h>
#include <klog.h>

/* Legacy virtio-pci I/O registers (offsets from the I/O BAR, MSI-X absent). */
#define VPCI_HOST_FEATURES   0x00   /* 32 r  */
#define VPCI_GUEST_FEATURES  0x04   /* 32 w  */
#define VPCI_QUEUE_PFN       0x08   /* 32 rw : vring address >> 12 */
#define VPCI_QUEUE_NUM       0x0C   /* 16 r  : device's fixed queue size */
#define VPCI_QUEUE_SEL       0x0E   /* 16 w  */
#define VPCI_QUEUE_NOTIFY    0x10   /* 16 w  */
#define VPCI_STATUS          0x12   /*  8 rw */
#define VPCI_ISR             0x13   /*  8 r  */
#define VPCI_CONFIG          0x14   /* device-specific config (no MSI-X) */

#define VSTAT_ACK        1
#define VSTAT_DRIVER     2
#define VSTAT_DRIVER_OK  4
#define VSTAT_FAILED  0x80

#define VIRTIO_VENDOR     0x1AF4
#define VIRTIO_DEV_BLK    0x1001    /* legacy/transitional block device */

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2       /* device writes this buffer (read requests) */

#define VRING_AVAIL_F_NO_INTERRUPT 1  /* "don't raise a completion IRQ" (we poll) */

#define VIRTIO_BLK_T_IN   0         /* read from device */
#define VIRTIO_BLK_T_OUT  1         /* write to device  */

#define VRING_ALIGN  4096u

struct vring_desc {
    uint64_t addr; uint32_t len; uint16_t flags; uint16_t next;
} __attribute__((packed));
struct vring_avail {
    uint16_t flags; uint16_t idx; uint16_t ring[];
} __attribute__((packed));
struct vring_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct vring_used {
    uint16_t flags; uint16_t idx; struct vring_used_elem ring[];
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type; uint32_t reserved; uint64_t sector;
} __attribute__((packed));

static struct {
    int      present;
    uint16_t io;                 /* I/O BAR base port */
    uint16_t qsz;                /* queue size (power of two) */
    uint64_t capacity;           /* in 512-byte sectors */
    uint8_t* vq;                 /* vring region (virt == phys) */
    uint64_t vq_pages;
    volatile struct vring_desc*  desc;
    volatile struct vring_avail* avail;
    volatile struct vring_used*  used;
    uint16_t last_used;          /* our view of used->idx */
    struct virtio_blk_req* hdr;  /* request header (one in flight) */
    volatile uint8_t* status;    /* request status byte */
} bd;

static inline uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

int virtio_blk_present(void)   { return bd.present; }
uint64_t virtio_blk_capacity(void) { return bd.capacity; }

int virtio_blk_init(void) {
    const pci_device_t* p = pci_find(VIRTIO_VENDOR, VIRTIO_DEV_BLK);
    if (!p) { KLOG_I("VBLK", "no legacy virtio-blk device\n"); return -1; }

    int is_io = 0;
    uint64_t bar0 = pci_bar_address(p, 0, &is_io);
    if (!is_io || bar0 == 0) { KLOG_E("VBLK", "BAR0 is not an I/O port region\n"); return -1; }
    bd.io = (uint16_t)bar0;
    pci_enable_bus_mastering(p);

    /* Reset, then walk the status handshake: ACK -> DRIVER -> (features) -> OK. */
    outb(bd.io + VPCI_STATUS, 0);
    outb(bd.io + VPCI_STATUS, VSTAT_ACK);
    outb(bd.io + VPCI_STATUS, VSTAT_ACK | VSTAT_DRIVER);
    /* Accept no optional features: basic read/write needs none. */
    (void)inl(bd.io + VPCI_HOST_FEATURES);
    outl(bd.io + VPCI_GUEST_FEATURES, 0);

    /* Queue 0: the device fixes the size; we allocate the vring for it. */
    outw(bd.io + VPCI_QUEUE_SEL, 0);
    uint16_t qsz = inw(bd.io + VPCI_QUEUE_NUM);
    if (qsz == 0 || (qsz & (qsz - 1))) {
        KLOG_E("VBLK", "bad queue size %u\n", qsz);
        outb(bd.io + VPCI_STATUS, VSTAT_FAILED); return -1;
    }
    bd.qsz = qsz;

    /* Legacy split-vring layout: desc | avail | (pad to 4096) used. */
    uint64_t desc_sz  = (uint64_t)qsz * sizeof(struct vring_desc);
    uint64_t avail_sz = 4 + 2 * (uint64_t)qsz + 2;      /* flags+idx+ring[]+used_event */
    uint64_t used_off = align_up(desc_sz + avail_sz, VRING_ALIGN);
    uint64_t used_sz  = 4 + 8 * (uint64_t)qsz + 2;      /* flags+idx+ring[]+avail_event */
    uint64_t total    = used_off + used_sz;
    bd.vq_pages = (total + 4095) / 4096;

    bd.vq = pmm_alloc_pages(bd.vq_pages);
    if (!bd.vq) { KLOG_E("VBLK", "vring alloc (%lu pages) failed\n",
                         (unsigned long)bd.vq_pages);
                  outb(bd.io + VPCI_STATUS, VSTAT_FAILED); return -1; }
    memset(bd.vq, 0, bd.vq_pages * 4096);
    bd.desc  = (volatile struct vring_desc*)(bd.vq);
    bd.avail = (volatile struct vring_avail*)(bd.vq + desc_sz);
    bd.used  = (volatile struct vring_used*)(bd.vq + used_off);
    bd.last_used = 0;
    /* We service requests by polling the used ring, so ask the device never to
     * raise a completion interrupt. Legacy INTx is level-triggered; an unacked
     * line would otherwise storm the PIC and wedge the machine. */
    bd.avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    /* Hand the device the vring by page-frame number, then go live. */
    outl(bd.io + VPCI_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)bd.vq >> 12));

    /* A page for the request header + status byte (one request in flight). */
    uint8_t* hp = pmm_alloc_page();
    if (!hp) { outb(bd.io + VPCI_STATUS, VSTAT_FAILED); return -1; }
    bd.hdr    = (struct virtio_blk_req*)hp;
    bd.status = (volatile uint8_t*)(hp + sizeof(struct virtio_blk_req));

    outb(bd.io + VPCI_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK);

    bd.capacity = (uint64_t)inl(bd.io + VPCI_CONFIG)
                | ((uint64_t)inl(bd.io + VPCI_CONFIG + 4) << 32);
    bd.present = 1;
    KLOG_I("VBLK", "legacy virtio-blk up: io=%x qsz=%u capacity=%lu sectors\n",
           bd.io, bd.qsz, (unsigned long)bd.capacity);
    return 0;
}

/* One synchronous, polled request. type is VIRTIO_BLK_T_IN/OUT. */
static int vblk_rw(uint64_t sector, void* buf, uint32_t count, int type) {
    if (!bd.present) return -ENODEV;
    if (count == 0) return 0;
    if (sector + count > bd.capacity) return -EINVAL;

    bd.hdr->type = (uint32_t)type;
    bd.hdr->reserved = 0;
    bd.hdr->sector = sector;
    *bd.status = 0xFF;                           /* overwritten by the device */

    /* Three-descriptor chain: header (r), data (r or w), status (w). */
    bd.desc[0].addr = (uint64_t)(uintptr_t)bd.hdr;
    bd.desc[0].len  = sizeof(struct virtio_blk_req);
    bd.desc[0].flags = VRING_DESC_F_NEXT; bd.desc[0].next = 1;

    bd.desc[1].addr = (uint64_t)(uintptr_t)buf;
    bd.desc[1].len  = count * VIRTIO_BLK_SECTOR;
    bd.desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    bd.desc[1].next = 2;

    bd.desc[2].addr = (uint64_t)(uintptr_t)bd.status;
    bd.desc[2].len  = 1;
    bd.desc[2].flags = VRING_DESC_F_WRITE; bd.desc[2].next = 0;

    /* Publish the head (desc 0) into the available ring and notify the device. */
    bd.avail->ring[bd.avail->idx % bd.qsz] = 0;
    __sync_synchronize();
    bd.avail->idx++;
    __sync_synchronize();
    outw(bd.io + VPCI_QUEUE_NOTIFY, 0);

    /* Poll the used ring. Bounded so a wedged device returns instead of hanging. */
    uint64_t spin = 0;
    while (bd.used->idx == bd.last_used) {
        if (++spin > 100000000ULL) { KLOG_E("VBLK", "request timeout\n"); return -EIO; }
        __asm__ volatile("pause");
    }
    __sync_synchronize();
    bd.last_used = bd.used->idx;

    if (*bd.status != 0) { KLOG_E("VBLK", "request status %u\n", *bd.status); return -EIO; }
    return 0;
}

int virtio_blk_read(uint64_t sector, void* buf, uint32_t count) {
    return vblk_rw(sector, buf, count, VIRTIO_BLK_T_IN);
}
int virtio_blk_write(uint64_t sector, const void* buf, uint32_t count) {
    return vblk_rw(sector, (void*)buf, count, VIRTIO_BLK_T_OUT);
}
