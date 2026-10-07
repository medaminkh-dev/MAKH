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
#define SYS_IOCTL       16
#define SYS_RT_SIGACTION   13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN   15
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_MPROTECT    10
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_CLOSE       3
#define SYS_GETCWD      79
#define SYS_CHDIR       80
#define SYS_GETPID      39
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_KILL        62

/* Phase 20-F: process groups / sessions (job control). */
#define SYS_SETPGID     109
#define SYS_GETPGRP     111
#define SYS_SETSID      112
#define SYS_GETPGID     121

#define SYS_MAKH_GETTICKS  0x200
#define SYS_MAKH_SLEEP_MS  0x201

/* mmap/mprotect protection bits and flags (Linux ABI subset, Phase 20-B). */
#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      (-1L)   /* mmap returns this (as a pointer) on failure */

/* Set up EFER.SCE, STAR/LSTAR/FMASK so `syscall` from ring 3 traps correctly. */
void syscall_init(void);

/* IA32_LSTAR assembly entry (usermode.asm) hands us the trapframe. */
uint64_t syscall_dispatch(trapframe_t* tf);

/* In-kernel convenience path (pointers are kernel pointers): used by the boot
 * self-check. Returns the same values the ring-3 path would. */
int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);

#endif /* MAKHOS_SYSCALL_H */
