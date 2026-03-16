#include <proc_internal.h>
#include <kernel.h>
#include <vga.h>

/**
 * =============================================================================
 * sched.c - Process Scheduler
 * =============================================================================
 * Implements round-robin scheduling with context switching.
 * =============================================================================
 */

void proc_yield(void) {
    // CRITICAL: Must be FIRST line - prevents context switch during interrupts
    if (in_interrupt_context)
        return;

    terminal_writestring("[PROC] proc_yield: ready_count=0x");
    phex(ready_queue.count);
    terminal_writestring(", current PID=0x");
    phex(current_process ? current_process->pid : 0xFFFF);
    terminal_writestring("\n");

    // Get next process from ready queue
    process_t *next = ready_dequeue();
    if (!next) {
        terminal_writestring("[PROC] ERROR: No ready processes!\n");
        return;
    }

    // Save old process (if any) and prepare for switch
    process_t *old = current_process;
    current_process = next;
    current_process->state = PROC_RUNNING;

    // If there was a previous process and it's still runnable, put it back
    if (old && old != next && old->state == PROC_RUNNING) {
        ready_enqueue(old);
    }

    terminal_writestring("[PROC] Switching PID 0x");
    phex(old ? old->pid : 0xFFFF);
    terminal_writestring(" -> PID 0x");
    phex(current_process->pid);
    terminal_writestring("\n");

    // Perform context switch
    context_switch(old ? &old->context : NULL, &current_process->context);
}
