#include <proc/pcb.h>
#include <kernel.h>
#include <vga.h>
#include <mm/kheap.h>
#include <lib/string.h>

/**
 * =============================================================================
 * kernel/proc/pcb/table.c - Process Table Management
 * =============================================================================
 * Phase 11: Process table array and PID bitmap allocation
 * =============================================================================
 */

// =============================================================================
// GLOBAL STATE
// =============================================================================

static process_t* process_table[MAX_PROCESSES];
static uint32_t process_count = 0;
static uint8_t pid_bitmap[PID_MAX / 8];

// =============================================================================
// PID ALLOCATION
// =============================================================================

static void pid_bitmap_init(void) {
    for (int i = 0; i < PID_MAX / 8; i++) {
        pid_bitmap[i] = 0;
    }
    // Reserve PID 0 for idle
    pid_bitmap[0] |= 1;
}

int32_t proc_alloc_pid(void) {
    for (int i = 1; i < PID_MAX; i++) {
        int byte_idx = i / 8;
        int bit_idx = i % 8;
        if (!(pid_bitmap[byte_idx] & (1 << bit_idx))) {
            pid_bitmap[byte_idx] |= (1 << bit_idx);
            return i;
        }
    }
    return -1;
}

void proc_reserve_pid(int32_t pid) {
    if (pid < 0 || pid >= PID_MAX) return;
    int byte_idx = pid / 8;
    int bit_idx = pid % 8;
    pid_bitmap[byte_idx] |= (1 << bit_idx);
}

void proc_free_pid(int32_t pid) {
    if (pid < 0 || pid >= PID_MAX) return;
    int byte_idx = pid / 8;
    int bit_idx = pid % 8;
    pid_bitmap[byte_idx] &= ~(1 << bit_idx);
}

// =============================================================================
// PROCESS TABLE
// =============================================================================

process_t* proc_find(int32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && (int32_t)process_table[i]->pid == pid) {
            return process_table[i];
        }
    }
    return NULL;
}

process_t* proc_table_alloc(void) {
    if (process_count >= MAX_PROCESSES) return NULL;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) {
            process_t* proc = kmalloc(sizeof(process_t));
            if (proc) {
                process_table[i] = proc;
                process_count++;
                return proc;
            }
        }
    }
    return NULL;
}

void proc_table_free(process_t* proc) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == proc) {
            process_table[i] = NULL;
            process_count--;
            kfree(proc);
            return;
        }
    }
}

// =============================================================================
// TABLE INITIALIZATION
// =============================================================================

void proc_table_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }
    process_count = 0;
    pid_bitmap_init();
}
