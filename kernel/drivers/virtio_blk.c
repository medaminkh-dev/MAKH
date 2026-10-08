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
#include <mm/vmm.h>
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

#define VBLK_MAX 4                   /* disks we will drive at once */

typedef struct vblk_dev {
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
} vblk_dev_t;

static vblk_dev_t bd[VBLK_MAX];
static int        nvblk;         /* number of disks brought up */

static inline uint64_t align_up(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }

int      virtio_blk_count(void) { return nvblk; }
int      virtio_blk_present(void) { return nvblk > 0; }
uint64_t virtio_blk_capacity(int unit) {
    return (unit >= 0 && unit < nvblk && bd[unit].present) ? bd[unit].capacity : 0;
}

/* Bring up one legacy virtio-blk PCI device into slot *d. Returns 0 or -1. */
static int vblk_init_one(const pci_device_t* p, vblk_dev_t* d) {
    int is_io = 0;
    uint64_t bar0 = pci_bar_address(p, 0, &is_io);
    if (!is_io || bar0 == 0) { KLOG_E("VBLK", "BAR0 is not an I/O port region\n"); return -1; }
    d->io = (uint16_t)bar0;
    pci_enable_bus_mastering(p);

    /* Reset, then walk the status handshake: ACK -> DRIVER -> (features) -> OK. */
    outb(d->io + VPCI_STATUS, 0);
    outb(d->io + VPCI_STATUS, VSTAT_ACK);
    outb(d->io + VPCI_STATUS, VSTAT_ACK | VSTAT_DRIVER);
    (void)inl(d->io + VPCI_HOST_FEATURES);       /* accept no optional features */
    outl(d->io + VPCI_GUEST_FEATURES, 0);

    outw(d->io + VPCI_QUEUE_SEL, 0);             /* queue 0, size fixed by device */
    uint16_t qsz = inw(d->io + VPCI_QUEUE_NUM);
    if (qsz == 0 || (qsz & (qsz - 1))) {
        KLOG_E("VBLK", "bad queue size %u\n", qsz);
        outb(d->io + VPCI_STATUS, VSTAT_FAILED); return -1;
    }
    d->qsz = qsz;

    /* Legacy split-vring layout: desc | avail | (pad to 4096) used. */
    uint64_t desc_sz  = (uint64_t)qsz * sizeof(struct vring_desc);
    uint64_t avail_sz = 4 + 2 * (uint64_t)qsz + 2;
    uint64_t used_off = align_up(desc_sz + avail_sz, VRING_ALIGN);
    uint64_t used_sz  = 4 + 8 * (uint64_t)qsz + 2;
    d->vq_pages = (used_off + used_sz + 4095) / 4096;

    void* vq_phys = pmm_alloc_pages(d->vq_pages);
    if (!vq_phys) { KLOG_E("VBLK", "vring alloc failed\n");
                  outb(d->io + VPCI_STATUS, VSTAT_FAILED); return -1; }
    d->vq = (uint8_t*)P2V((uint64_t)(uintptr_t)vq_phys);   /* CPU view through the HHDM */
    memset(d->vq, 0, d->vq_pages * 4096);
    d->desc  = (volatile struct vring_desc*)(d->vq);
    d->avail = (volatile struct vring_avail*)(d->vq + desc_sz);
    d->used  = (volatile struct vring_used*)(d->vq + used_off);
    d->last_used = 0;
    /* Polled: ask the device never to raise a (level-triggered) completion IRQ. */
    d->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    /* The device takes a physical page frame number for the vring. */
    outl(d->io + VPCI_QUEUE_PFN, (uint32_t)((uint64_t)(uintptr_t)vq_phys >> 12));

    void* hp_phys = pmm_alloc_page();            /* request header + status byte */
    if (!hp_phys) { outb(d->io + VPCI_STATUS, VSTAT_FAILED); return -1; }
    uint8_t* hp = (uint8_t*)P2V((uint64_t)(uintptr_t)hp_phys);
    d->hdr    = (struct virtio_blk_req*)hp;
    d->status = (volatile uint8_t*)(hp + sizeof(struct virtio_blk_req));

    outb(d->io + VPCI_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_DRIVER_OK);

    d->capacity = (uint64_t)inl(d->io + VPCI_CONFIG)
                | ((uint64_t)inl(d->io + VPCI_CONFIG + 4) << 32);
    d->present = 1;
    KLOG_I("VBLK", "disk up: io=%x qsz=%u capacity=%lu sectors\n",
           d->io, d->qsz, (unsigned long)d->capacity);
    return 0;
}

int virtio_blk_init(void) {
    nvblk = 0;
    int n = pci_device_count();
    for (int i = 0; i < n && nvblk < VBLK_MAX; i++) {
        const pci_device_t* p = pci_get(i);
        if (p && p->vendor_id == VIRTIO_VENDOR && p->device_id == VIRTIO_DEV_BLK)
            if (vblk_init_one(p, &bd[nvblk]) == 0) nvblk++;
    }
    if (nvblk == 0) { KLOG_I("VBLK", "no legacy virtio-blk device\n"); return -1; }
    return 0;
}

/* One synchronous, polled request on unit `u`. type is VIRTIO_BLK_T_IN/OUT. */
static int vblk_rw(int u, uint64_t sector, void* buf, uint32_t count, int type) {
    if (u < 0 || u >= nvblk || !bd[u].present) return -ENODEV;
    vblk_dev_t* d = &bd[u];
    if (count == 0) return 0;
    if (sector + count > d->capacity) return -EINVAL;

    d->hdr->type = (uint32_t)type;
    d->hdr->reserved = 0;
    d->hdr->sector = sector;
    *d->status = 0xFF;                           /* overwritten by the device */

    /* Three-descriptor chain: header (r), data (r or w), status (w). The device
     * consumes physical addresses, so translate each kernel pointer (the driver's
     * own HHDM buffers and the caller's data buffer alike) via the page tables. */
    d->desc[0].addr = vmm_get_physical((uint64_t)(uintptr_t)d->hdr);
    d->desc[0].len  = sizeof(struct virtio_blk_req);
    d->desc[0].flags = VRING_DESC_F_NEXT; d->desc[0].next = 1;

    d->desc[1].addr = vmm_get_physical((uint64_t)(uintptr_t)buf);
    d->desc[1].len  = count * VIRTIO_BLK_SECTOR;
    d->desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    d->desc[1].next = 2;

    d->desc[2].addr = vmm_get_physical((uint64_t)(uintptr_t)d->status);
    d->desc[2].len  = 1;
    d->desc[2].flags = VRING_DESC_F_WRITE; d->desc[2].next = 0;

    /* Publish the head (desc 0) into the available ring and notify the device. */
    d->avail->ring[d->avail->idx % d->qsz] = 0;
    __sync_synchronize();
    d->avail->idx++;
    __sync_synchronize();
    outw(d->io + VPCI_QUEUE_NOTIFY, 0);

    /* Poll the used ring. Bounded so a wedged device returns instead of hanging. */
    uint64_t spin = 0;
    while (d->used->idx == d->last_used) {
        if (++spin > 100000000ULL) { KLOG_E("VBLK", "request timeout\n"); return -EIO; }
        __asm__ volatile("pause");
    }
    __sync_synchronize();
    d->last_used = d->used->idx;

    if (*d->status != 0) { KLOG_E("VBLK", "request status %u\n", *d->status); return -EIO; }
    return 0;
}

int virtio_blk_read(int unit, uint64_t sector, void* buf, uint32_t count) {
    return vblk_rw(unit, sector, buf, count, VIRTIO_BLK_T_IN);
}
int virtio_blk_write(int unit, uint64_t sector, const void* buf, uint32_t count) {
    return vblk_rw(unit, sector, (void*)buf, count, VIRTIO_BLK_T_OUT);
}
