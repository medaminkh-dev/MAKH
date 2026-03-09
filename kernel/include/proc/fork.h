#ifndef MAKHOS_PROC_FORK_H
#define MAKHOS_PROC_FORK_H

/**
 * =============================================================================
 * proc/fork.h - Fork System Call Implementation
 * =============================================================================
 * Phase 12: fork() system call for process creation
 * 
 * Provides:
 *   - proc_fork() - create child process copy
 *   - Memory copy helpers
 * =============================================================================
 */

#include <types.h>
#include <proc/pcb.h>

// =============================================================================
// FORK API
// =============================================================================

/**
 * proc_fork - Fork current process (create child copy)
 * 
 * Creates a new process that is an exact copy of the current process.
 * Parent receives child's PID, child receives 0.
 * 
 * @return: Child's PID (parent) / 0 (child) / -1 (error)
 */
int proc_fork(void);

/**
 * copy_kernel_stack - Copy parent kernel stack to child
 * @parent: Parent process
 * @child: Child process
 * @return: 0 on success, -1 on failure
 */
int copy_kernel_stack(process_t* parent, process_t* child);

/**
 * copy_user_memory - Copy user memory to child
 * @parent: Parent process
 * @child: Child process
 * @return: 0 on success
 */
int copy_user_memory(process_t* parent, process_t* child);

#endif // MAKHOS_PROC_FORK_H
