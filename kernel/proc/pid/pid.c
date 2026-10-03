/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#include <proc_internal.h>
#include <kernel.h>
#include <vga.h>
#include <irq.h>

/**
 * =============================================================================
 * pid.c - PID Management with Bitmap
 * =============================================================================
 * Allocates PIDs using a bitmap for efficient reuse.
 * Supports up to 32768 PIDs (PID_MAX = 32768).
 * =============================================================================
 */

#define PID_BITMAP_SIZE (PID_MAX / 32)  // 32768/32 = 1024 uint32_t entries

static uint32_t pid_bitmap[PID_BITMAP_SIZE];
static uint32_t pid_cursor = 2;   /* next PID to try (rotates, like Linux) */

void pid_init(void) {
    for (int i = 0; i < PID_BITMAP_SIZE; i++) {
        pid_bitmap[i] = 0;
    }
    
    // Reserve PID 0 and 1
    pid_reserve(0);
    pid_reserve(1);
    
    terminal_writestring("[PID] Bitmap initialized (max=");
    phex(PID_MAX);
    terminal_writestring(")\n");
}

uint32_t pid_alloc(void) {
    /* Atomic scan+claim: concurrent creators must never get the same PID. */
    /* The cursor rotates through the PID space instead of restarting at 2,
     * so a just-freed PID is not handed out again immediately: a stale PID
     * held by a joiner/debugger then misses instead of hitting a new thread. */
    irqflags_t f = local_irq_save();
    for (uint32_t n = 0; n < PID_MAX - 2; n++) {
        uint32_t pid = pid_cursor;
        pid_cursor = (pid_cursor + 1 >= PID_MAX) ? 2 : pid_cursor + 1;
        uint32_t idx = pid / 32;
        uint32_t bit = pid % 32;
        if (!(pid_bitmap[idx] & (1u << bit))) {
            pid_bitmap[idx] |= (1u << bit);
            local_irq_restore(f);
            return pid;
        }
    }
    local_irq_restore(f);

    terminal_writestring("[PID] ERROR: No PIDs available!\n");
    return 0;  // 0 is invalid (reserved for idle)
}

void pid_free(uint32_t pid) {
    if (pid >= PID_MAX) return;
    if (pid == 0 || pid == 1) return;  // Never free idle or init

    irqflags_t f = local_irq_save();
    pid_bitmap[pid / 32] &= ~(1u << (pid % 32));
    local_irq_restore(f);
}

void pid_reserve(uint32_t pid) {
    if (pid >= PID_MAX) return;
    
    uint32_t idx = pid / 32;
    uint32_t bit = pid % 32;
    
    pid_bitmap[idx] |= (1 << bit);
}
