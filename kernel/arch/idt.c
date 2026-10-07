/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
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
#include <sched.h>
#include <klog.h>
#include <cmdline.h>
#include <proc.h>
#include <kfuzz.h>
#include <arch/usermode.h>
#include <mm/vmspace.h>
#include <signal.h>

static void exception_report(registers_t* regs) __attribute__((noreturn));

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
    // Note: We don't set up an IDT gate for syscall since x86_64 uses
    // the syscall/sysret instructions instead of int 0x80. The syscall
    // mechanism is configured via MSRs (STAR, LSTAR, FMASK) in syscall_init().
    
    
    // Load IDT
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt;
    
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
    
    terminal_writestring("[IDT] Loaded 32 exception gates + 16 IRQ gates\n");
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
    
    // Handle IRQs (vectors 32-47) separately
    if (vector >= 32 && vector < 48) {
        irq_handler(regs);
        return;
    }

    /* Phase 20-A-2: a write to a copy-on-write page (after fork) is resolved by
     * handing this space a private copy and retrying — not a fault. This fires
     * both for a ring-3 write and for a kernel copy_to_user into the current
     * process's COW page, so it must run before the uaccess fixup below. */
    if (vector == 14 && proc_current() && proc_current()->is_user &&
        proc_current()->aspace) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        if (vmspace_cow_fault((address_space_t*)proc_current()->aspace, cr2) == 0)
            return;                      /* COW resolved: retry the instruction */
    }

    /* Phase 16: a fault inside copy_from/to_user resumes at the uaccess fixup
     * label, which aborts the copy cleanly and returns -EFAULT to the caller. */
    uint64_t fixup = uaccess_fixup(regs->rip);
    if (fixup) { regs->rip = fixup; return; }

    /* Phase 16: a fault taken in ring 3 kills the user program, never the
     * kernel. (regs->cs carries the privilege level of the faulting code.) */
    if (vector < 32 && (regs->cs & 3) == 3 && usermode_active())
        usermode_fault(vector);
    if (vector < 32 && (regs->cs & 3) == 3 && proc_current() && proc_current()->is_user) {
        extern void proc_user_fault(long vector);
        proc_user_fault(vector);              /* terminate the process (SIGSEGV) */
    }

    /* Phase 15: if a KFUZZ target is running, a CPU fault (vectors 0-31) is a
     * finding, not a fatal event: hand it to the sandbox, which records the
     * reproducing seed and longjmps back to the fuzz harness (never returns). */
    if (vector < 32 && kfuzz_in_sandbox()) kfuzz_report_fault(regs);

    exception_report(regs);
}

/*
 * exception_report - dump everything we know about a fatal CPU exception and
 * stop. Under the test harness (makh.test) we power QEMU off with status 3 so
 * a fault fails the run immediately instead of hanging until the timeout.
 */
static void exception_report(registers_t* regs) {
    uint8_t vector = (uint8_t)regs->int_no;
    __asm__ volatile("cli");

    kprintf("\n!!! EXCEPTION CAUGHT !!!\n");
    kprintf("Exception: %s (vector %u)\n",
            vector < 32 ? exception_messages[vector] : "Interrupt", vector);
    if (regs->err_code != 0xDEADBEEF)
        kprintf("Error code: 0x%llx\n", (unsigned long long)regs->err_code);
    kprintf("RIP=%p RSP=%p RBP=%p RFLAGS=%llx\n", (void*)regs->rip, (void*)regs->rsp,
            (void*)regs->rbp, (unsigned long long)regs->rflags);
    kprintf("RAX=%p RBX=%p RCX=%p RDX=%p\n", (void*)regs->rax, (void*)regs->rbx,
            (void*)regs->rcx, (void*)regs->rdx);
    kprintf("RSI=%p RDI=%p R8=%p  R9=%p\n", (void*)regs->rsi, (void*)regs->rdi,
            (void*)regs->r8, (void*)regs->r9);

    if (vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("Page fault address: %p\n", (void*)cr2);
    } else if (vector == 1) {
        uint64_t dr6;
        __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
        kprintf("DR6=%llx (watchpoint hit; RIP is the instruction AFTER the write)\n",
                (unsigned long long)dr6);
    }

    process_t* cur = proc_current();
    if (cur) kprintf("Thread: '%s' pid %u\n", cur->name, cur->pid);

    kprintf("  #-  %p  (faulting RIP)\n", (void*)regs->rip);
    kernel_backtrace_from(regs->rbp);

    if (cmdline_has("makh.test")) qemu_debug_exit(3);
    kprintf("\nSystem halted.\n");
    kernel_halt();
}

/* -------------------------------------------------------------------------- */
/* Phase 14: registered IRQ handlers                                          */
/* -------------------------------------------------------------------------- */

static struct {
    irq_fn_t fn;
    void*    ctx;
    uint64_t count;
} irq_table[16];

int irq_register(uint8_t irq, irq_fn_t fn, void* ctx) {
    if (irq >= 16 || !fn) return -1;
    if (irq == IRQ_TIMER || irq == IRQ_KEYBOARD) return -1;  /* built-in */

    irqflags_t f = local_irq_save();
    irq_table[irq].fn = fn;
    irq_table[irq].ctx = ctx;
    irq_table[irq].count = 0;
    local_irq_restore(f);

    /* Slave-PIC lines (8-15) only reach the CPU through the cascade (IRQ2). */
    if (irq >= 8) pic_unmask_irq(IRQ_CASCADE);
    pic_unmask_irq(irq);
    return 0;
}

void irq_unregister(uint8_t irq) {
    if (irq >= 16) return;
    pic_mask_irq(irq);
    irq_table[irq].fn = NULL;
    irq_table[irq].ctx = NULL;
}

uint64_t irq_get_count(uint8_t irq) {
    return irq < 16 ? irq_table[irq].count : 0;
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
            // Phase 14: dispatch to a registered driver handler (e.g. NIC).
            // It must acknowledge the device *before* the EOI below, since
            // PCI INTx lines are level-triggered.
            if (irq < 16 && irq_table[irq].fn) {
                irq_table[irq].count++;
                irq_table[irq].fn(irq_table[irq].ctx);
            }
            break;
    }
    
    // Send End of Interrupt to PIC before any reschedule, so the PIC can
    // deliver the next IRQ to whichever thread we switch to.
    pic_send_eoi(irq);

    // Phase 20-D: if this IRQ interrupted a user process in ring 3 and a fatal
    // signal is now pending (e.g. the keyboard IRQ just turned Ctrl+C into a
    // SIGINT for the foreground group), terminate it here. This catches even a
    // CPU-bound program that never makes a syscall. Only when interrupted in
    // ring 3 — never mid-syscall — so no kernel operation is cut short.
    if ((regs->cs & 3) == 3) signal_check_and_die();

    // IRQ tail: if the timer (or a wakeup) marked the current thread for
    // preemption, switch now. Runs on the interrupted thread's kernel stack;
    // when this thread is scheduled again the switch unwinds back here and
    // isr_common's iretq resumes the interrupted code.
    sched_preempt_if_needed();
}
