#ifndef MAKHOS_USER_H
#define MAKHOS_USER_H

#include <types.h>

// User mode segment selectors (with RPL=3)
#define USER_CS     0x18 | 3  // User code segment + ring 3
#define USER_DS     0x20 | 3  // User data segment + ring 3

// User mode flags
#define USER_IF     0x0200     // Interrupt flag (enable interrupts)

// Function to enter user mode (never returns)
void enter_usermode(uint64_t entry, uint64_t stack, uint64_t arg) __attribute__((noreturn));

#endif
