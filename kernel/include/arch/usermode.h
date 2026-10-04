/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - arch/usermode.h
 * Ring-3 entry/exit and safe access to user memory (Phase 16).
 */

#ifndef MAKHOS_ARCH_USERMODE_H
#define MAKHOS_ARCH_USERMODE_H

#include <types.h>

/*
 * Register state saved on kernel entry. Field order matches the push order in
 * usermode.asm (syscall_entry) exactly: the lowest-addressed field (r15) is
 * where the trapframe pointer points. On the syscall path int_no holds the
 * syscall number and rax is overwritten with the return value.
 */
typedef struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t rip, cs, rflags, rsp, ss;
} trapframe_t;

/* User address space window (single shared address space for now; per-process
 * isolation arrives with the VMM rework in Phase 17). Chosen well above the
 * identity-mapped RAM so user pages never alias kernel mappings. */
#define USER_CODE_BASE   0x0000200000000000ULL
#define USER_STACK_TOP   0x0000200000080000ULL   /* 512 KiB window            */

/* Reason a user program stopped, reported by run_user_program(). */
typedef enum {
    USER_EXITED = 0,   /* called exit(); *status holds the code             */
    USER_FAULTED = 1,  /* killed by a CPU fault; *status holds the vector   */
} user_stop_t;

/*
 * Load `code` (len bytes) into a fresh user code page, set up a user stack,
 * drop to ring 3 at its entry, and run until it calls exit() or faults.
 * Returns USER_EXITED or USER_FAULTED and writes the code/vector to *status.
 * Faults in ring 3 are contained: they never reach the kernel panic path.
 */
user_stop_t run_user_program(const void* code, size_t len, long* status);

/* Assembly trampolines (usermode.asm). */
void syscall_entry(void);                       /* IA32_LSTAR target          */
void enter_user_mode(uint64_t rip, uint64_t rsp) __attribute__((noreturn));

/* Lifecycle + exception-handler hooks (usermode.c). */
void usermode_init(void);                       /* per-CPU block + GS base     */
int  usermode_active(void);                     /* 1 while in run_user_program */
void usermode_exit(long code) __attribute__((noreturn));   /* sys_exit path    */
void usermode_fault(long vector) __attribute__((noreturn));/* ring-3 CPU fault */

/* -------- safe user-memory access (uaccess.c) -------- */
/* Copy to/from a user pointer, guarding against a bad address: on a fault the
 * fixup table unwinds the copy and these return -EFAULT instead of crashing. */
long copy_from_user(void* dst, const void* user_src, size_t n);
long copy_to_user(void* user_dst, const void* src, size_t n);
int  user_range_ok(const void* user_ptr, size_t n);   /* canonical user range */

/* Page-fault hook: if `fault_rip` is inside a uaccess copy, returns the
 * recovery address to resume at (so the copy aborts cleanly); else 0. */
uint64_t uaccess_fixup(uint64_t fault_rip);

#endif /* MAKHOS_ARCH_USERMODE_H */
