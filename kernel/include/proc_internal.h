#ifndef MAKHOS_PROC_INTERNAL_H
#define MAKHOS_PROC_INTERNAL_H

#include <proc.h>

/**
 * =============================================================================
 * proc_internal.h - Internal Process Management Header
 * =============================================================================
 * This header exposes internal variables and functions to other process
 * management modules. Only files inside kernel/proc/ should include this.
 * =============================================================================
 */

// -----------------------------------------------------------------------------
// GLOBAL VARIABLES (declared as extern)
// -----------------------------------------------------------------------------

extern volatile int in_interrupt_context;
extern process_t *current_process;
extern process_list_t ready_queue;
extern process_list_t all_processes;

// -----------------------------------------------------------------------------
// STACKS (declared as extern arrays)
// -----------------------------------------------------------------------------

extern uint8_t idle_stack[IDLE_STACK_SIZE];
extern uint8_t init_stack[INIT_STACK_SIZE];

// -----------------------------------------------------------------------------
// FUNCTION DECLARATIONS - All Processes List
// -----------------------------------------------------------------------------

void all_list_add(process_t *proc);
void all_list_remove(process_t *proc);

// -----------------------------------------------------------------------------
// FUNCTION DECLARATIONS - Ready Queue
// -----------------------------------------------------------------------------

void ready_enqueue(process_t *proc);
void ready_remove(process_t *proc);
process_t *ready_dequeue(void);

// -----------------------------------------------------------------------------
// FUNCTION DECLARATIONS - PID Management
// -----------------------------------------------------------------------------

uint32_t pid_alloc(void);      // Returns 0 on error
void pid_free(uint32_t pid);
void pid_reserve(uint32_t pid);
void pid_init(void);

// -----------------------------------------------------------------------------
// FUNCTION DECLARATIONS - Process Table
// -----------------------------------------------------------------------------

process_t *proc_table_alloc(void);
void proc_table_free(process_t *proc);
process_t *proc_find(int32_t pid);
void proc_table_init(void);
uint32_t proc_get_count(void);

// -----------------------------------------------------------------------------
// FUNCTION DECLARATIONS - Process Tree
// -----------------------------------------------------------------------------

void proc_add_child(process_t *parent, process_t *child);
void proc_remove_child(process_t *child);
void proc_reparent_orphans(process_t *dead_parent);

// -----------------------------------------------------------------------------
// HELPER FUNCTIONS
// -----------------------------------------------------------------------------

void phex(uint64_t v);

#endif // MAKHOS_PROC_INTERNAL_H
