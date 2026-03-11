#ifndef MAKHOS_SYSCALL_H
#define MAKHOS_SYSCALL_H

/**
 * =============================================================================
 * syscall/syscall.h - System Call Interface
 * =============================================================================
 * FIXES APPLIED:
 *   BUG#7: Added SYS_WAITPID (61) and SYS_WAIT (4) syscall numbers
 * =============================================================================
 */

#include <types.h>

// =============================================================================
// SYSCALL NUMBERS
// =============================================================================

#define SYS_EXIT        1
#define SYS_WRITE       2
#define SYS_WAIT        4    // FIX BUG#7: wait (any child)
#define SYS_GETPID      5
#define SYS_SLEEP       6
#define SYS_GETTICKS    7
#define SYS_GETPPID     8
#define SYS_GETPRIORITY 9
#define SYS_SETPRIORITY 10
#define SYS_FORK        57
#define SYS_WAITPID     61   // FIX BUG#7: waitpid (specific PID)
#define MAX_SYSCALLS    256

// =============================================================================
// SYSCALL HANDLER TYPES
// =============================================================================

typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// =============================================================================
// SYSCALL API
// =============================================================================

void syscall_init(void);

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif // MAKHOS_SYSCALL_H
