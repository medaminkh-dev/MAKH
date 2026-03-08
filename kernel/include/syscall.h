#ifndef MAKHOS_SYSCALL_H
#define MAKHOS_SYSCALL_H

#include <types.h>

// System call numbers
#define SYS_EXIT        0
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_GETPID      5
#define SYS_SLEEP       6
#define SYS_GETTICKS    7
#define SYS_GETPPID     8
#define SYS_GETPRIORITY 9
#define SYS_SETPRIORITY 10

// Process management
#define SYS_FORK       57   // Standard Linux fork syscall number

// Maximum number of syscalls
#define MAX_SYSCALLS    64

// Syscall function type
typedef uint64_t (*syscall_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// Public API (for kernel use)
void syscall_init(void);
uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, 
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

// Convenience macros for making syscalls from kernel
#define syscall0(num) \
    syscall_handler(num, 0, 0, 0, 0, 0)

#define syscall1(num, a1) \
    syscall_handler(num, (uint64_t)(a1), 0, 0, 0, 0)

#define syscall2(num, a1, a2) \
    syscall_handler(num, (uint64_t)(a1), (uint64_t)(a2), 0, 0, 0)

#define syscall3(num, a1, a2, a3) \
    syscall_handler(num, (uint64_t)(a1), (uint64_t)(a2), (uint64_t)(a3), 0, 0)

#endif
