#include <proc_internal.h>
#include <mm/kheap.h>
#include <kernel.h>
#include <vga.h>
#include <drivers/timer.h>

/**
 * =============================================================================
 * exit.c - Process Termination
 * =============================================================================
 * Handles process exit, zombie state, and orphan reparenting.
 * Phase 11: Added statistics and proper cleanup.
 * =============================================================================
 */

void proc_exit(int code) {
    if (!current_process) {
        terminal_writestring("[PROC] ERROR: No current process to exit\n");
        for(;;);
    }

    terminal_writestring("[PROC] Exit PID 0x");
    phex(current_process->pid);
    terminal_writestring(" (");
    terminal_writestring(current_process->name);
    terminal_writestring(") code=0x");
    phex((uint64_t)(unsigned)code);
    terminal_writestring("\n");

    // Store exit code and time
    current_process->exit_code = code;
    current_process->exit_time = timer_get_ticks();
    
    // Calculate total CPU time used
    if (current_process->creation_time > 0) {
        current_process->cpu_time_used += (current_process->exit_time - current_process->creation_time);
    }

    // Remove from ready queue
    ready_remove(current_process);

    // Reparent orphans to init
    if (current_process->child_count > 0) {
        proc_reparent_orphans(current_process);
    }

    // Remove from parent's children list
    if (current_process->parent_pid != 0 && current_process->parent_pid != current_process->pid) {
        proc_remove_child(current_process);
    }

    // Convert to ZOMBIE
    current_process->state = PROC_ZOMBIE;

    // Schedule next process
    proc_yield();

    // Should never reach here
    for (;;) __asm__ volatile("hlt");
}
