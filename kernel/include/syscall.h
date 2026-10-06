/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
#ifndef MAKHOS_SYSCALL_H
#define MAKHOS_SYSCALL_H

#include <types.h>
#include <arch/usermode.h>

/*
 * System call numbers. The common ones match the Linux x86_64 ABI so that, in
 * a later phase, programs built against a Linux-targeting libc can run with
 * little or no change. MAKH-specific calls live above 0x200 to stay clear of
 * the Linux range.
 */
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_LSEEK       8
#define SYS_CLOSE       3
#define SYS_GETPID      39
#define SYS_EXIT        60

#define SYS_MAKH_GETTICKS  0x200
#define SYS_MAKH_SLEEP_MS  0x201

/* Set up EFER.SCE, STAR/LSTAR/FMASK so `syscall` from ring 3 traps correctly. */
void syscall_init(void);

/* IA32_LSTAR assembly entry (usermode.asm) hands us the trapframe. */
uint64_t syscall_dispatch(trapframe_t* tf);

/* In-kernel convenience path (pointers are kernel pointers): used by the boot
 * self-check. Returns the same values the ring-3 path would. */
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

#endif /* MAKHOS_SYSCALL_H */
