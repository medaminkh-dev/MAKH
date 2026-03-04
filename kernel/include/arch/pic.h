/**
 * MakhOS - pic.h
 * 8259A Programmable Interrupt Controller header
 */

#ifndef MAKHOS_PIC_H
#define MAKHOS_PIC_H

#include <types.h>

// PIC ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

// PIC commands
#define PIC_EOI       0x20
#define PIC_INIT      0x11
#define PIC_8086_MODE 0x01

// IRQ numbers
#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_CASCADE   2
#define IRQ_COM2      3
#define IRQ_COM1      4
#define IRQ_LPT2      5
#define IRQ_FLOPPY    6
#define IRQ_LPT1      7
#define IRQ_CMOS      8
#define IRQ_FREE1     9
#define IRQ_FREE2     10
#define IRQ_FREE3     11
#define IRQ_PS2MOUSE  12
#define IRQ_FPU       13
#define IRQ_PRIMARY   14
#define IRQ_SECONDARY 15

// Functions
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
void pic_disable(void);

#endif
