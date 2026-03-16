#include <proc_internal.h>

/**
 * =============================================================================
 * ready_api.c - Public Ready Queue API
 * =============================================================================
 * Provides public functions for adding processes to the ready queue.
 * =============================================================================
 */

void proc_add_to_ready(process_t *proc) {
    if (!proc) return;
    ready_enqueue(proc);
}
