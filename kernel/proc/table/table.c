#include <proc_internal.h>
#include <mm/kheap.h>
#include <kernel.h>
#include <vga.h>
#include <lib/string.h>

/**
 * =============================================================================
 * table.c - Process Table Management
 * =============================================================================
 * Manages the process table array for O(1) process lookup.
 * Supports up to MAX_PROCESSES (256) concurrent processes.
 * =============================================================================
 */

#define MAX_PROCESSES 256

static process_t* process_table[MAX_PROCESSES];
static uint32_t process_count = 0;

void proc_table_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }
    process_count = 0;
    
    terminal_writestring("[TABLE] Process table initialized, max=");
    phex(MAX_PROCESSES);
    terminal_writestring("\n");
}

process_t* proc_table_alloc(void) {
    if (process_count >= MAX_PROCESSES) {
        terminal_writestring("[TABLE] ERROR: Process table full\n");
        return NULL;
    }
    
    // Find first free slot
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) {
            // Allocate actual PCB (this will be filled by create.c)
            process_t* proc = (process_t*)kmalloc(sizeof(process_t));
            if (!proc) {
                terminal_writestring("[TABLE] ERROR: kmalloc failed\n");
                return NULL;
            }
            process_table[i] = proc;
            process_count++;
            return proc;
        }
    }
    
    terminal_writestring("[TABLE] ERROR: No free slots\n");
    return NULL;
}

void proc_table_free(process_t* proc) {
    if (!proc) return;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == proc) {
            process_table[i] = NULL;
            process_count--;
            kfree(proc);
            return;
        }
    }
}

process_t* proc_find(int32_t pid) {
    if (pid < 0 || pid >= PID_MAX) return NULL;
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && (int32_t)process_table[i]->pid == pid) {
            return process_table[i];
        }
    }
    return NULL;
}

uint32_t proc_get_count(void) {
    return process_count;
}
