#include <proc_internal.h>
#include <kernel.h>
#include <vga.h>

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
    // Start from PID 2 (0 and 1 are reserved)
    for (uint32_t pid = 2; pid < PID_MAX; pid++) {
        uint32_t idx = pid / 32;
        uint32_t bit = pid % 32;
        
        if (!(pid_bitmap[idx] & (1 << bit))) {
            pid_bitmap[idx] |= (1 << bit);
            return pid;
        }
    }
    
    terminal_writestring("[PID] ERROR: No PIDs available!\n");
    return 0;  // 0 is invalid (reserved for idle)
}

void pid_free(uint32_t pid) {
    if (pid >= PID_MAX) return;
    if (pid == 0 || pid == 1) return;  // Never free idle or init
    
    uint32_t idx = pid / 32;
    uint32_t bit = pid % 32;
    
    if (pid_bitmap[idx] & (1 << bit)) {
        pid_bitmap[idx] &= ~(1 << bit);
    }
}

void pid_reserve(uint32_t pid) {
    if (pid >= PID_MAX) return;
    
    uint32_t idx = pid / 32;
    uint32_t bit = pid % 32;
    
    pid_bitmap[idx] |= (1 << bit);
}
