/**
 * MakhOS - idt.h
 * Interrupt Descriptor Table header
 */

#ifndef MAKHOS_IDT_H
#define MAKHOS_IDT_H

#include <types.h>

// IDT entry structure (64-bit)
typedef struct {
    uint16_t offset_low;    // bits 0-15 of handler
    uint16_t selector;      // code segment selector (0x08 for kernel)
    uint8_t  ist;            // ONLY BITS 0-2! bits 3-7 MUST be 0
    uint8_t  type_attr;      // type and attributes
    uint16_t offset_mid;     // bits 16-31 of handler
    uint32_t offset_high;    // bits 32-63 of handler
    uint32_t reserved;       // MUST BE 0
} __attribute__((packed)) idt_entry_t;

// IDT pointer for lidt instruction
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr_t;

// Flag bits for type_attr byte
// Bit 7: Present, Bits 6-5: DPL, Bit 4: 0, Bits 3-0: Type
#define IDT_PRESENT     (1 << 7)    // Bit 7: Present
#define IDT_DPL0        (0 << 5)    // Bits 6-5: DPL 0 (kernel)
#define IDT_DPL3        (3 << 5)    // Bits 6-5: DPL 3 (user)
#define IDT_INTERRUPT   (0xE)       // Bits 3-0: 64-bit Interrupt gate
#define IDT_TRAP        (0xF)       // Bits 3-0: 64-bit Trap gate

// Common combinations
#define IDT_FLAGS_KERNEL_INT (IDT_PRESENT | IDT_DPL0 | IDT_INTERRUPT)  // 0x8E

// Registers saved by interrupt stubs
typedef struct {
    // General purpose registers (saved by stub)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    
    // Interrupt info (pushed by stub)
    uint64_t int_no;
    uint64_t err_code;
    
    // CPU-pushed registers (by interrupt)
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) registers_t;

// Functions
void idt_init(void);
void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t flags);
void idt_enable_interrupts(void);
void idt_disable_interrupts(void);

// Common handler (called from assembly)
void exception_handler(registers_t* regs);

// IRQ handler (called from exception_handler for vectors 32-47)
void irq_handler(registers_t* regs);

// Assembly ISR stub table
extern uint64_t isr_stub_table[256];

#endif
