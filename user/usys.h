/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* Tiny freestanding syscall wrappers for MAKH user programs (Phase 20-A/B). */
#ifndef MAKH_USYS_H
#define MAKH_USYS_H
#define SYS_READ      0
#define SYS_WRITE     1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_IOCTL     16
#define SYS_MMAP      9
#define SYS_MPROTECT  10
#define SYS_MUNMAP    11
#define SYS_BRK       12
#define SYS_GETCWD    79
#define SYS_CHDIR     80
#define SYS_GETPID    39
#define SYS_FORK      57
#define SYS_EXECVE    59
#define SYS_WAIT4     61
#define SYS_EXIT      60
#define SYS_SETPGID   109
#define SYS_GETPGRP   111
#define SYS_SETSID    112
#define SYS_GETPGID   121

#define TIOCGPGRP     0x540F
#define TIOCSPGRP     0x5410

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static inline long usyscall(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
/* 4-argument form: the syscall ABI's 4th argument lives in r10, not rcx. */
static inline long usyscall4(long n, long a, long b, long c, long d) {
    register long r10 __asm__("r10") = d;
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long uwrite(int fd, const void* buf, unsigned long n) {
    return usyscall(SYS_WRITE, fd, (long)buf, (long)n);
}
static inline long uread(int fd, void* buf, unsigned long n) {
    return usyscall(SYS_READ, fd, (long)buf, (long)n);
}
static inline long ugetpid(void) { return usyscall(SYS_GETPID, 0, 0, 0); }
static inline void* umap(unsigned long len, int prot, int flags) {
    return (void*)usyscall4(SYS_MMAP, 0, (long)len, prot, flags);
}
static inline long umunmap(void* addr, unsigned long len) {
    return usyscall(SYS_MUNMAP, (long)addr, (long)len, 0);
}
static inline long umprotect(void* addr, unsigned long len, int prot) {
    return usyscall(SYS_MPROTECT, (long)addr, (long)len, prot);
}
static inline long ubrk(unsigned long newbrk) {
    return usyscall(SYS_BRK, (long)newbrk, 0, 0);
}
static inline long uopen(const char* path, int flags) {
    return usyscall(SYS_OPEN, (long)path, flags, 0);
}
static inline long uclose(int fd) { return usyscall(SYS_CLOSE, fd, 0, 0); }
static inline long uchdir(const char* path) {
    return usyscall(SYS_CHDIR, (long)path, 0, 0);
}
static inline long ugetcwd(char* buf, unsigned long size) {
    return usyscall(SYS_GETCWD, (long)buf, (long)size, 0);
}
static inline long ufork(void) { return usyscall(SYS_FORK, 0, 0, 0); }
static inline long uexecve(const char* path, char* const* argv, char* const* envp) {
    return usyscall(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}
static inline long uwaitpid(int pid, int* status) {
    return usyscall(SYS_WAIT4, pid, (long)status, 0);
}

/* --- job control (Phase 20-F) --- */
static inline long usetpgid(int pid, int pgid) {
    return usyscall(SYS_SETPGID, pid, pgid, 0);
}
static inline long ugetpgid(int pid) { return usyscall(SYS_GETPGID, pid, 0, 0); }
static inline long ugetpgrp(void)     { return usyscall(SYS_GETPGRP, 0, 0, 0); }
static inline long usetsid(void)      { return usyscall(SYS_SETSID, 0, 0, 0); }
static inline long uioctl(int fd, unsigned long req, void* arg) {
    return usyscall(SYS_IOCTL, fd, (long)req, (long)arg);
}
/* tcsetpgrp/tcgetpgrp: hand the terminal to a process group, or read it back. */
static inline long utcsetpgrp(int fd, int pgid) {
    return uioctl(fd, TIOCSPGRP, &pgid);
}
static inline int utcgetpgrp(int fd) {
    int pgid = -1;
    if (uioctl(fd, TIOCGPGRP, &pgid) < 0) return -1;
    return pgid;
}
#endif
