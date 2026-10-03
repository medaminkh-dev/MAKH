/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_IRQ_H
#define MAKHOS_IRQ_H

/**
 * =============================================================================
 * irq.h - Local interrupt enable/disable (single-CPU critical sections)
 * =============================================================================
 * On a uniprocessor, the only concurrency is the interrupt handlers (timer,
 * keyboard, ...) and the preemption they trigger. Clearing IF turns any region
 * into a critical section. Always restore the *saved* flags rather than blindly
 * doing sti, so nested critical sections compose correctly.
 * =============================================================================
 */

typedef unsigned long irqflags_t;

static inline irqflags_t local_irq_save(void) {
    irqflags_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void local_irq_restore(irqflags_t flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

#endif /* MAKHOS_IRQ_H */
