/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - pci.c
 * PCI configuration space access and bus enumeration.
 */

#include <drivers/pci.h>
#include <kernel.h>
#include <klog.h>
#include <irq.h>

static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count = 0;

/* -------------------------------------------------------------------------- */
/* Configuration space access                                                 */
/* -------------------------------------------------------------------------- */

static uint32_t cfg_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (1u << 31)                 /* enable bit */
         | ((uint32_t)bus  << 16)
         | ((uint32_t)(slot & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (off & 0xFC);              /* dword aligned */
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    irqflags_t f = local_irq_save();  /* address+data is a two-step sequence */
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, off));
    uint32_t v = inl(PCI_CONFIG_DATA);
    local_irq_restore(f);
    return v;
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint16_t)(pci_read32(bus, slot, func, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint8_t)(pci_read32(bus, slot, func, off) >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    irqflags_t f = local_irq_save();
    outl(PCI_CONFIG_ADDRESS, cfg_address(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, v);
    local_irq_restore(f);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    uint32_t old = pci_read32(bus, slot, func, off);
    int shift = (off & 2) * 8;
    old &= ~(0xFFFFu << shift);
    old |= (uint32_t)v << shift;
    pci_write32(bus, slot, func, off, old);
}

/* -------------------------------------------------------------------------- */
/* Enumeration                                                                */
/* -------------------------------------------------------------------------- */

static void probe_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read16(bus, slot, func, PCI_VENDOR_ID);
    if (vendor == 0xFFFF) return;           /* no function here */
    if (device_count >= PCI_MAX_DEVICES) return;

    pci_device_t* d = &devices[device_count++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor_id = vendor;
    d->device_id = pci_read16(bus, slot, func, PCI_DEVICE_ID);

    uint32_t cr = pci_read32(bus, slot, func, PCI_CLASS_REVISION);
    d->revision   = (uint8_t)(cr & 0xFF);
    d->prog_if    = (uint8_t)(cr >> 8);
    d->subclass   = (uint8_t)(cr >> 16);
    d->class_code = (uint8_t)(cr >> 24);
    d->header_type = pci_read8(bus, slot, func, PCI_HEADER_TYPE);
    d->irq_line    = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);

    int nbars = ((d->header_type & 0x7F) == 0) ? 6 : 2;
    for (int i = 0; i < 6; i++) {
        d->bar[i] = (i < nbars) ? pci_read32(bus, slot, func, (uint8_t)(PCI_BAR0 + i * 4)) : 0;
    }

    KLOG_I("PCI", "%02x:%02x.%u %04x:%04x class %02x:%02x irq %u\n",
           bus, slot, func, d->vendor_id, d->device_id,
           d->class_code, d->subclass, d->irq_line);
}

void pci_init(void) {
    device_count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_read16((uint8_t)bus, slot, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
            probe_function((uint8_t)bus, slot, 0);
            /* Multi-function device? (bit 7 of header type) */
            if (pci_read8((uint8_t)bus, slot, 0, PCI_HEADER_TYPE) & 0x80) {
                for (uint8_t func = 1; func < 8; func++) {
                    probe_function((uint8_t)bus, slot, func);
                }
            }
        }
    }
    KLOG_I("PCI", "%d device(s) found\n", device_count);
}

int pci_device_count(void) { return device_count; }

const pci_device_t* pci_get(int index) {
    if (index < 0 || index >= device_count) return NULL;
    return &devices[index];
}

const pci_device_t* pci_find(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device)
            return &devices[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Driver helpers                                                             */
/* -------------------------------------------------------------------------- */

void pci_enable_bus_mastering(const pci_device_t* d) {
    uint16_t cmd = pci_read16(d->bus, d->slot, d->func, PCI_COMMAND);
    cmd |= PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    cmd &= (uint16_t)~PCI_CMD_INTX_DISABLE;
    pci_write16(d->bus, d->slot, d->func, PCI_COMMAND, cmd);
}

uint64_t pci_bar_address(const pci_device_t* d, int bar, int* is_io) {
    if (bar < 0 || bar > 5) return 0;
    uint32_t v = d->bar[bar];
    if (v & 1) {                        /* I/O space BAR */
        if (is_io) *is_io = 1;
        return v & ~0x3u;
    }
    if (is_io) *is_io = 0;
    uint64_t addr = v & ~0xFu;
    if (((v >> 1) & 0x3) == 0x2 && bar < 5) {   /* 64-bit memory BAR */
        addr |= (uint64_t)d->bar[bar + 1] << 32;
    }
    return addr;
}
