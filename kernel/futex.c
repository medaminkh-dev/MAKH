/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - futex.c
 * A hashed-bucket futex (Phase 20-L). Waiters park on the bucket for their
 * word's physical address; a wake pulls up to N off that bucket. Hash
 * collisions only cause harmless spurious wakeups — the caller re-checks the
 * word and waits again, exactly as the futex contract allows.
 *
 * On this single-CPU kernel the check-then-sleep in futex_wait is made atomic
 * against futex_wake by holding interrupts off across both: nothing else can
 * run in between, so no wakeup is lost.
 */
#include <futex.h>
#include <proc_internal.h>
#include <sched.h>
#include <irq.h>
#include <errno.h>
#include <arch/usermode.h>        /* copy_from_user */
#include <mm/vmspace.h>

#define NBUCKETS 64
static wait_queue_t bucket[NBUCKETS];

void futex_init(void) {
    for (int i = 0; i < NBUCKETS; i++) wq_init(&bucket[i]);
}

/* Translate a user virtual address to the physical address of the word, using
 * the caller's address space. 0 means unmapped/invalid. */
static uint64_t uaddr_phys(uint64_t va) {
    process_t* cur = current_process;
    if (!cur || !cur->aspace) return 0;
    uint64_t frame = vmspace_phys((address_space_t*)cur->aspace, va & ~0xFFFULL);
    if (!frame) return 0;
    return frame + (va & 0xFFF);
}

static int bucket_of(uint64_t phys) { return (int)((phys >> 2) % NBUCKETS); }

long futex_wait(uint64_t uaddr, uint32_t val, uint64_t timeout_ticks) {
    uint64_t phys = uaddr_phys(uaddr);
    if (!phys) return -EFAULT;
    int b = bucket_of(phys);

    irqflags_t f = local_irq_save();
    uint32_t cur = 0;
    if (copy_from_user(&cur, (const void*)(uintptr_t)uaddr, sizeof(cur)) < 0) {
        local_irq_restore(f);
        return -EFAULT;
    }
    if (cur != val) {                 /* changed already: don't sleep (fast path) */
        local_irq_restore(f);
        return -EAGAIN;
    }
    int rc = sched_wait_event(&bucket[b], timeout_ticks, f);   /* restores f */
    if (rc == 2) return -EINTR;
    if (rc == 1) return -ETIMEDOUT;
    return 0;                         /* woken by futex_wake (maybe spurious) */
}

long futex_wake(uint64_t uaddr, int n) {
    uint64_t phys = uaddr_phys(uaddr);
    if (!phys) return -EFAULT;
    int b = bucket_of(phys);
    int woken = 0;
    irqflags_t f = local_irq_save();
    while (woken < n && bucket[b].head) { wq_wake_one(&bucket[b]); woken++; }
    local_irq_restore(f);
    return woken;
}
