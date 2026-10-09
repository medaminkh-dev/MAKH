/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * =============================================================================
 * e1000.c - Intel 8254x Gigabit Ethernet driver
 * =============================================================================
 * - Registers via MMIO (BAR0), mapped uncached with vmm_map_mmio().
 * - Legacy RX/TX descriptor rings in PMM frames. PMM frames are identity-
 *   mapped, so a buffer's virtual address IS its DMA (physical) address.
 * - RX: the IRQ handler only acks the interrupt (reading ICR) and wakes netd;
 *   netd drains the ring through dev->poll() with net_lock held.
 * - TX: copy the frame into the next descriptor's buffer and bump TDT, under a
 *   short IRQ-off critical section (callers hold net_lock anyway).
 * =============================================================================
 */

#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <arch/idt.h>
#include <net/net.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <irq.h>
#include <kernel.h>
#include <klog.h>
#include <lib/string.h>

/* ---------------------------------------------------------------- registers */

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_SLU        (1u << 6)
#define CTRL_RST        (1u << 26)

#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)     /* accept broadcast */
#define RCTL_SECRC      (1u << 26)     /* strip Ethernet CRC */
#define RCTL_BSIZE_2048 (0u << 16)

#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12

#define IMS_TXDW   (1u << 0)
#define IMS_LSC    (1u << 2)
#define IMS_RXDMT0 (1u << 4)
#define IMS_RXO    (1u << 6)
#define IMS_RXT0   (1u << 7)

#define RXD_STAT_DD  0x01
#define RXD_STAT_EOP 0x02
#define TXD_CMD_EOP  0x01
#define TXD_CMD_IFCS 0x02
#define TXD_CMD_RS   0x08
#define TXD_STAT_DD  0x01

#define NUM_RX 32
#define NUM_TX 32
#define BUF_SIZE 2048

typedef struct __attribute__((packed)) rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

typedef struct __attribute__((packed)) tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc_t;

/* ---------------------------------------------------------------- state */

static struct {
    int         present;
    uintptr_t   mmio;
    const pci_device_t* pci;
    rx_desc_t*  rx;
    tx_desc_t*  tx;
    uint8_t*    rx_buf[NUM_RX];
    uint8_t*    tx_buf[NUM_TX];
    uint32_t    rx_cur;
    uint32_t    tx_cur;
    netdev_t    dev;
} nic;

static inline uint32_t rd(uint32_t reg) {
    return *(volatile uint32_t*)(nic.mmio + reg);
}
static inline void wr(uint32_t reg, uint32_t v) {
    *(volatile uint32_t*)(nic.mmio + reg) = v;
}

int e1000_present(void) { return nic.present; }
uint8_t e1000_irq_line(void) { return nic.pci ? nic.pci->irq_line : 0xFF; }

/* ---------------------------------------------------------------- EEPROM / MAC */

static int eeprom_read(uint8_t addr, uint16_t* out) {
    wr(REG_EERD, ((uint32_t)addr << 8) | 1u);           /* START */
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4)) { *out = (uint16_t)(v >> 16); return 0; }   /* DONE */
    }
    return -1;
}

static void read_mac(uint8_t mac[ETH_ALEN]) {
    uint16_t w0, w1, w2;
    if (eeprom_read(0, &w0) == 0 && eeprom_read(1, &w1) == 0 && eeprom_read(2, &w2) == 0) {
        mac[0] = (uint8_t)w0; mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)w1; mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)w2; mac[5] = (uint8_t)(w2 >> 8);
        return;
    }
    /* Fallback: receive-address register 0, loaded from EEPROM at reset. */
    uint32_t lo = rd(REG_RAL0), hi = rd(REG_RAH0);
    mac[0] = (uint8_t)lo; mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16); mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)hi; mac[5] = (uint8_t)(hi >> 8);
}

/* ---------------------------------------------------------------- IRQ */

static void e1000_irq(void* ctx) {
    (void)ctx;
    /* Reading ICR acknowledges (clears) the causes and deasserts the
     * level-triggered INTx line - this must happen before the PIC EOI. */
    uint32_t cause = rd(REG_ICR);
    if (cause) net_rx_notify();
}

/* ---------------------------------------------------------------- netdev ops */

static int e1000_send(netdev_t* dev, const void* frame, size_t len) {
    if (len > BUF_SIZE) { dev->tx_dropped++; return -1; }

    irqflags_t f = local_irq_save();
    uint32_t i = nic.tx_cur;
    tx_desc_t* d = &nic.tx[i];

    /* Ring full: the descriptor we'd reuse hasn't been transmitted yet. */
    if (!(d->status & TXD_STAT_DD)) {
        local_irq_restore(f);
        dev->tx_dropped++;
        return -1;
    }

    memcpy(nic.tx_buf[i], frame, len);
    d->addr = vmm_get_physical((uint64_t)(uintptr_t)nic.tx_buf[i]);
    d->length = (uint16_t)len;
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;
    nic.tx_cur = (i + 1) % NUM_TX;
    wr(REG_TDT, nic.tx_cur);
    local_irq_restore(f);

    dev->tx_packets++;
    dev->tx_bytes += len;
    return 0;
}

static void e1000_poll(netdev_t* dev) {
    for (;;) {
        uint32_t i = nic.rx_cur;
        rx_desc_t* d = &nic.rx[i];
        if (!(d->status & RXD_STAT_DD)) break;

        uint16_t len = d->length;
        if ((d->status & RXD_STAT_EOP) && d->errors == 0 && len <= BUF_SIZE) {
            dev->rx_packets++;
            dev->rx_bytes += len;
            net_input(dev, nic.rx_buf[i], len);
        } else {
            dev->rx_dropped++;
        }

        /* Hand the descriptor back to the NIC. */
        d->status = 0;
        nic.rx_cur = (i + 1) % NUM_RX;
        wr(REG_RDT, i);
    }
}

/* ---------------------------------------------------------------- init */

/* Rings and packet buffers are PMM frames: the NIC DMAs to/from their physical
 * addresses, but the CPU touches them through the higher-half direct map so the
 * driver is correct under a user CR3 (socket syscalls reach here). Store the
 * HHDM pointer; translate back to physical with vmm_get_physical() for the NIC. */
static int alloc_rings(void) {
    void* rxp = pmm_alloc_page();              /* 32 * 16 B = 512 B */
    void* txp = pmm_alloc_page();
    if (!rxp || !txp) return -1;
    nic.rx = (rx_desc_t*)P2V((uint64_t)(uintptr_t)rxp);
    nic.tx = (tx_desc_t*)P2V((uint64_t)(uintptr_t)txp);
    memset(nic.rx, 0, 4096);
    memset(nic.tx, 0, 4096);

    /* Two 2KB buffers per 4KB frame. */
    for (int i = 0; i < NUM_RX; i += 2) {
        void* page = pmm_alloc_page();
        if (!page) return -1;
        nic.rx_buf[i] = (uint8_t*)P2V((uint64_t)(uintptr_t)page);
        nic.rx_buf[i + 1] = nic.rx_buf[i] + BUF_SIZE;
    }
    for (int i = 0; i < NUM_TX; i += 2) {
        void* page = pmm_alloc_page();
        if (!page) return -1;
        nic.tx_buf[i] = (uint8_t*)P2V((uint64_t)(uintptr_t)page);
        nic.tx_buf[i + 1] = nic.tx_buf[i] + BUF_SIZE;
    }
    return 0;
}

int e1000_init(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    const pci_device_t* p = pci_find(E1000_VENDOR, E1000_DEV_82540EM);
    if (!p) p = pci_find(E1000_VENDOR, E1000_DEV_82545EM);
    if (!p) return -1;
    nic.pci = p;

    int is_io = 0;
    uint64_t bar0 = pci_bar_address(p, 0, &is_io);
    if (is_io || bar0 == 0) { KLOG_E("E1000", "BAR0 is not MMIO\n"); return -1; }
    uint64_t mmio_va = vmm_map_mmio(bar0, 128 * 1024);   /* higher-half, shared */
    if (mmio_va == 0) { KLOG_E("E1000", "MMIO map failed\n"); return -1; }
    nic.mmio = (uintptr_t)mmio_va;

    pci_enable_bus_mastering(p);

    /* Reset the MAC, then wait for the reset bit to self-clear. */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000 && (rd(REG_CTRL) & CTRL_RST); i++) { }
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);

    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);        /* link up */

    memset(&nic.dev, 0, sizeof(nic.dev));
    read_mac(nic.dev.mac);

    /* Program our unicast address into RA0 with Address Valid set. */
    wr(REG_RAL0, (uint32_t)nic.dev.mac[0] | ((uint32_t)nic.dev.mac[1] << 8) |
                 ((uint32_t)nic.dev.mac[2] << 16) | ((uint32_t)nic.dev.mac[3] << 24));
    wr(REG_RAH0, (uint32_t)nic.dev.mac[4] | ((uint32_t)nic.dev.mac[5] << 8) | (1u << 31));

    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);   /* no multicast */

    if (alloc_rings() != 0) { KLOG_E("E1000", "out of memory\n"); return -1; }

    /* RX ring: all descriptors owned by the NIC. */
    for (int i = 0; i < NUM_RX; i++) {
        nic.rx[i].addr = vmm_get_physical((uint64_t)(uintptr_t)nic.rx_buf[i]);
        nic.rx[i].status = 0;
    }
    uint64_t rx_phys = vmm_get_physical((uint64_t)(uintptr_t)nic.rx);
    wr(REG_RDBAL, (uint32_t)rx_phys);
    wr(REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(REG_RDLEN, NUM_RX * sizeof(rx_desc_t));
    wr(REG_RDH, 0);
    wr(REG_RDT, NUM_RX - 1);
    nic.rx_cur = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    /* TX ring: mark every descriptor "done" so the first sends find room. */
    for (int i = 0; i < NUM_TX; i++) {
        nic.tx[i].addr = vmm_get_physical((uint64_t)(uintptr_t)nic.tx_buf[i]);
        nic.tx[i].status = TXD_STAT_DD;
    }
    uint64_t tx_phys = vmm_get_physical((uint64_t)(uintptr_t)nic.tx);
    wr(REG_TDBAL, (uint32_t)tx_phys);
    wr(REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(REG_TDLEN, NUM_TX * sizeof(tx_desc_t));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    nic.tx_cur = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0Fu << TCTL_CT_SHIFT) | (0x40u << TCTL_COLD_SHIFT));
    wr(REG_TIPG, 0x0060200Au);

    /* Network device registration. */
    nic.dev.name[0] = 'e'; nic.dev.name[1] = 't'; nic.dev.name[2] = 'h';
    nic.dev.name[3] = '0'; nic.dev.name[4] = 0;
    nic.dev.ip = ip;
    nic.dev.netmask = netmask;
    nic.dev.gateway = gateway;
    nic.dev.send = e1000_send;
    nic.dev.poll = e1000_poll;
    nic.dev.up = 1;

    irq_register(p->irq_line, e1000_irq, &nic.dev);
    wr(REG_IMS, IMS_RXT0 | IMS_RXO | IMS_RXDMT0 | IMS_LSC);

    nic.present = 1;
    netdev_register(&nic.dev);
    netdev_set_default(&nic.dev);

    KLOG_I("E1000", "up: mmio %p irq %u link %s\n", (void*)nic.mmio, p->irq_line,
           (rd(REG_STATUS) & 2) ? "up" : "down");
    return 0;
}
