/**
 * MakhOS - pic.c
 * 8259A Programmable Interrupt Controller implementation
 */

#include <arch/pic.h>
#include <kernel.h>
#include <vga.h>

/**
 * pic_init - Initialize the 8259A PIC
 * 
 * Remaps the PIC vectors to avoid conflict with CPU exceptions (0-31).
 * Master PIC: vectors 0x20-0x27 (32-39)
 * Slave PIC: vectors 0x28-0x2F (40-47)
 */
void pic_init(void) {
    terminal_writestring("[PIC] Initializing 8259A...\n");
    
    // ICW1: Start initialization sequence, cascade mode, expect ICW4
    outb(PIC1_COMMAND, PIC_INIT);
    io_wait();
    outb(PIC2_COMMAND, PIC_INIT);
    io_wait();
    
    // ICW2: Vector offset
    // Master: 0x20 (32) - vectors 32-39
    // Slave: 0x28 (40) - vectors 40-47
    outb(PIC1_DATA, 0x20);
    io_wait();
    outb(PIC2_DATA, 0x28);
    io_wait();
    
    // ICW3: Tell master about slave at IRQ2 (bit 2), tell slave its cascade identity (2)
    outb(PIC1_DATA, 0x04);  // bit 2 = slave at IRQ2
    io_wait();
    outb(PIC2_DATA, 0x02);  // cascade identity = 2
    io_wait();
    
    // ICW4: 8086/88 mode
    outb(PIC1_DATA, PIC_8086_MODE);
    io_wait();
    outb(PIC2_DATA, PIC_8086_MODE);
    io_wait();
    
    // Restore saved masks (or use 0xFF to mask all)
    outb(PIC1_DATA, 0xFF);  // Mask all IRQs initially
    outb(PIC2_DATA, 0xFF);
    
    terminal_writestring("[PIC] Remapped to vectors 0x20-0x2F\n");
}

/**
 * pic_send_eoi - Send End of Interrupt signal to PIC
 * @irq: The IRQ number that was handled (0-15)
 * 
 * For IRQs from slave (8-15), must send EOI to both slave and master.
 * For IRQs from master (0-7), send EOI to master only.
 */
void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        // Send EOI to slave
        outb(PIC2_COMMAND, PIC_EOI);
    }
    // Send EOI to master (for all IRQs)
    outb(PIC1_COMMAND, PIC_EOI);
}

/**
 * pic_mask_irq - Mask (disable) a specific IRQ
 * @irq: The IRQ number to mask (0-15)
 * 
 * Sets the bit in the mask register to disable the IRQ.
 */
void pic_mask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) | (1 << irq);
    outb(port, value);
}

/**
 * pic_unmask_irq - Unmask (enable) a specific IRQ
 * @irq: The IRQ number to unmask (0-15)
 * 
 * Clears the bit in the mask register to enable the IRQ.
 */
void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    
    value = inb(port) & ~(1 << irq);
    outb(port, value);
    
    char buf[32];
    terminal_writestring("[PIC] Unmasked IRQ ");
    uint64_to_string(irq < 8 ? irq : irq + 8, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

/**
 * pic_disable - Disable the PIC completely
 * 
 * Masks all IRQs on both PICs. Useful when switching to APIC.
 */
void pic_disable(void) {
    // Mask all IRQs on both PICs
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    
    terminal_writestring("[PIC] Disabled all IRQs\n");
}
