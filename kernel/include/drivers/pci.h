/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_PCI_H
#define MAKHOS_PCI_H

#include <types.h>

/**
 * =============================================================================
 * pci.h - PCI bus enumeration (legacy configuration mechanism #1)
 * =============================================================================
 * Configuration space is reached through I/O ports 0xCF8 (address) and 0xCFC
 * (data). pci_init() scans every bus/slot/function once and caches what it
 * finds; drivers then look devices up by vendor/device ID.
 * =============================================================================
 */

#define PCI_CONFIG_ADDRESS  0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* Standard configuration space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_CLASS_REVISION  0x08
#define PCI_HEADER_TYPE     0x0E
#define PCI_BAR0            0x10
#define PCI_INTERRUPT_LINE  0x3C

/* Command register bits */
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEM_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)
#define PCI_CMD_INTX_DISABLE (1 << 10)

#define PCI_MAX_DEVICES 32

typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

/* Raw configuration space access */
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t v);
void     pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t v);

/* Enumeration */
void pci_init(void);
int  pci_device_count(void);
const pci_device_t* pci_get(int index);
const pci_device_t* pci_find(uint16_t vendor, uint16_t device);

/* Helpers for drivers */
void     pci_enable_bus_mastering(const pci_device_t* dev);
uint64_t pci_bar_address(const pci_device_t* dev, int bar, int* is_io);

#endif /* MAKHOS_PCI_H */
