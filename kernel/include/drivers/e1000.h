/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_E1000_H
#define MAKHOS_E1000_H

#include <types.h>

/**
 * e1000.h - Intel 8254x (e1000) Gigabit Ethernet driver.
 * Targets QEMU's default NIC (82540EM, 8086:100E) and close relatives.
 */

#define E1000_VENDOR        0x8086
#define E1000_DEV_82540EM   0x100E
#define E1000_DEV_82545EM   0x100F
#define E1000_DEV_82574L    0x10D3

/* Probe PCI for an e1000; if found, initialise it, register it as a netdev
 * with the given IPv4 configuration (host byte order) and return 0. */
int e1000_init(uint32_t ip, uint32_t netmask, uint32_t gateway);

/* 1 if an e1000 was found and brought up. */
int e1000_present(void);

/* PIC line the NIC interrupts on (valid when e1000_present()). */
uint8_t e1000_irq_line(void);

#endif /* MAKHOS_E1000_H */
