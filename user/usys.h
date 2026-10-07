/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Tiny freestanding syscall wrappers for MAKH user programs (Phase 20-A). */
#ifndef MAKH_USYS_H
#define MAKH_USYS_H
#define SYS_WRITE  1
#define SYS_EXIT   60
#define SYS_GETPID 39
static inline long usyscall(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long uwrite(int fd, const void* buf, unsigned long n) {
    return usyscall(SYS_WRITE, fd, (long)buf, (long)n);
}
static inline long ugetpid(void) { return usyscall(SYS_GETPID, 0, 0, 0); }
#endif
