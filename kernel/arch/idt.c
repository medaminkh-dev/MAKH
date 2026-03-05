/**
 * MakhOS - idt.c
 * Interrupt Descriptor Table implementation
 */

#include <arch/idt.h>
#include <arch/pic.h>
#include <drivers/timer.h>
#include <drivers/keyboard.h>
#include <kernel.h>
#include <vga.h>

static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;

// Exception messages for vectors 0-31
static const char* exception_messages[] = {
    "Division By Zero",              // 0
    "Debug",                          // 1
    "Non Maskable Interrupt",         // 2
    "Breakpoint",                     // 3
    "Overflow",                       // 4
    "Bound Range Exceeded",           // 5
    "Invalid Opcode",                 // 6
    "Device Not Available",           // 7
    "Double Fault",                    // 8
    "Coprocessor Segment Overrun",    // 9
    "Invalid TSS",                     // 10
    "Segment Not Present",             // 11
    "Stack-Segment Fault",             // 12
    "General Protection Fault",        // 13
    "Page Fault",                       // 14
    "Reserved",                         // 15
    "x87 Floating-Point Exception",    // 16
    "Alignment Check",                  // 17
    "Machine Check",                    // 18
    "SIMD Floating-Point Exception",   // 19
    "Virtualization Exception",         // 20
    "Control Protection Exception",     // 21
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Security Exception",               // 30
    "Reserved"                          // 31
};

void idt_init(void) {
    terminal_writestring("[IDT] Initializing Interrupt Descriptor Table...\n");
    
    // Clear all IDT entries
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0x08;  // Kernel code segment
        idt[i].ist = 0;  // No IST, bits 3-7 must be 0
        idt[i].type_attr = 0;  // No gate set
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].reserved = 0;
    }
    
    // Set gates for vectors 0-31 (exceptions)
    for (int i = 0; i < 32; i++) {
        if (isr_stub_table[i] != 0) {
            idt_set_gate(i, isr_stub_table[i], IDT_FLAGS_KERNEL_INT);
        }
    }
    
    // Set gates for vectors 32-47 (IRQs)
    for (int i = 32; i < 48; i++) {
        if (isr_stub_table[i] != 0) {
            idt_set_gate(i, isr_stub_table[i], IDT_FLAGS_KERNEL_INT);
        }
    }
    
    // Set test gates for vectors 0x30 and 0x48
    if (isr_stub_table[0x30] != 0) {
        idt_set_gate(0x30, isr_stub_table[0x30], IDT_FLAGS_KERNEL_INT);
    }
    if (isr_stub_table[0x48] != 0) {
        idt_set_gate(0x48, isr_stub_table[0x48], IDT_FLAGS_KERNEL_INT);
    }
    
    // Load IDT
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;
    
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    
    terminal_writestring("[IDT] Loaded 32 exception gates + 16 IRQ gates + syscall gate\n");
}

void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t flags) {
    idt[vector].offset_low = handler & 0xFFFF;
    idt[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].selector = 0x08;  // Kernel code segment
    idt[vector].type_attr = flags;  // P|DPL|0|Type (0x8E for kernel int)
    idt[vector].ist = 0;  // IST=0 (bits 3-7 must be 0)
    idt[vector].reserved = 0;
}

void idt_enable_interrupts(void) {
    __asm__ volatile("sti");
    terminal_writestring("[IDT] Interrupts enabled\n");
}

void idt_disable_interrupts(void) {
    __asm__ volatile("cli");
}

// Common exception handler called from assembly
void exception_handler(registers_t* regs) {
    uint8_t vector = regs->int_no;
    char buf[32];
    
    // Handle IRQs (vectors 32-47) separately
    if (vector >= 32 && vector < 48) {
        irq_handler(regs);
        return;
    }
    
    terminal_writestring("\n!!! EXCEPTION CAUGHT !!!\n");
    
    // Print exception name for vectors 0-31
    if (vector < 32) {
        terminal_writestring("Exception: ");
        terminal_writestring(exception_messages[vector]);
        terminal_writestring(" (vector ");
        uint64_to_string(vector, buf);
        terminal_writestring(buf);
        terminal_writestring(")\n");
    } else {
        terminal_writestring("Interrupt: vector ");
        uint64_to_string(vector, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
    }
    
    // Print error code if present (use 0xDEADBEEF as sentinel for no error code)
    if (regs->err_code != 0xDEADBEEF) {
        terminal_writestring("Error code: 0x");
        uint64_to_hex(regs->err_code, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
    }
    
    // Print CPU state
    terminal_writestring("RIP: 0x");
    uint64_to_hex(regs->rip, buf);
    terminal_writestring(buf);
    terminal_writestring("  CS: 0x");
    uint64_to_hex(regs->cs, buf);
    terminal_writestring(buf);
    terminal_writestring("\n");
    
    // For page fault (vector 14), get CR2
    if (vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_writestring("Page fault address: 0x");
        uint64_to_hex(cr2, buf);
        terminal_writestring(buf);
        terminal_writestring("\n");
    }
    
    // Halt the system
    terminal_writestring("\nSystem halted.\n");
    for(;;) {
        __asm__ volatile("hlt");
    }
}

/**
 * irq_handler - Handle hardware interrupts (IRQs)
 * @regs: CPU register state (including interrupt number)
 * 
 * Called from assembly ISR stubs for vectors 32-47 (IRQ0-IRQ15).
 * Dispatches to appropriate device handler and sends EOI to PIC.
 */
void irq_handler(registers_t* regs) {
    uint8_t vector = regs->int_no;
    uint8_t irq = vector - 32;  // Convert vector to IRQ number
    
    // Handle spurious IRQs (IRQ7 and IRQ15)
    // These can occur if an IRQ is triggered during PIC initialization
    if (irq == 7) {
        // Check if this is a real IRQ7 or spurious
        // Read ISR register from master PIC
        outb(PIC1_COMMAND, 0x0B);  // Read ISR
        uint8_t isr = inb(PIC1_COMMAND);
        if (!(isr & 0x80)) {
            // Spurious IRQ7 - don't send EOI
            return;
        }
    } else if (irq == 15) {
        // Check if this is a real IRQ15 or spurious
        outb(PIC2_COMMAND, 0x0B);  // Read ISR from slave
        uint8_t isr = inb(PIC2_COMMAND);
        if (!(isr & 0x80)) {
            // Spurious IRQ15 - only send EOI to master (for cascade)
            outb(PIC1_COMMAND, PIC_EOI);
            return;
        }
    }
    
    // Dispatch to appropriate handler based on IRQ number
    switch (irq) {
        case IRQ_TIMER:
            // Timer interrupt (IRQ0)
            timer_handler(regs);
            break;
            
        case IRQ_KEYBOARD:
            // Keyboard interrupt (IRQ1)
            keyboard_handler(regs);
            break;
            
        default:
            // Other IRQs - just acknowledge for now
            break;
    }
    
    // Send End of Interrupt to PIC
    pic_send_eoi(irq);
}
