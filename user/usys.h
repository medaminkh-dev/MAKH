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
#define SYS_RT_SIGACTION   13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN   15
#define SYS_ARCH_PRCTL     158
#define SYS_NANOSLEEP      35
#define SYS_GETTIMEOFDAY   96
#define SYS_CLOCK_GETTIME  228
#define SYS_GETRANDOM      318
#define SYS_STAT           4
#define SYS_FSTAT          5
#define SYS_FCNTL          72
#define SYS_GETDENTS64     217
#define SYS_PIPE           22
#define SYS_DUP2           33
#define SYS_CLONE          56
#define SYS_FUTEX          202
#define SYS_SET_TID_ADDRESS 218
#define SYS_READV          19
#define SYS_WRITEV         20
#define SYS_EXIT_GROUP     231
#define SYS_KILL      62

struct iovec { void* iov_base; unsigned long iov_len; };

/* clone flags + futex ops (match kernel). */
#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_CHILD_CLEARTID 0x00200000
#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

/* st_mode bits + fcntl + getdents (match kernel fs/vfs.h). */
#define S_IFMT   0170000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define F_GETFL  3
#define F_SETFL  4
#define DT_DIR   4
#define DT_REG   8

struct stat {
    unsigned long st_dev, st_ino, st_nlink;
    unsigned int  st_mode, st_uid, st_gid, __pad0;
    unsigned long st_rdev;
    long st_size, st_blksize, st_blocks;
    long st_atime_sec, st_atime_nsec;
    long st_mtime_sec, st_mtime_nsec;
    long st_ctime_sec, st_ctime_nsec;
    long __unused[3];
};
struct linux_dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1
struct timespec { long tv_sec; long tv_nsec; };
struct timeval  { long tv_sec; long tv_usec; };

#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003

#define TIOCGPGRP     0x540F
#define TIOCSPGRP     0x5410

/* Signals + sigaction/sigprocmask constants (match kernel signal.h). */
#define SIGINT    2
#define SIGKILL   9
#define SIGSEGV   11
#define SIGTERM   15
#define SIGCHLD   17
#define SIG_DFL   0
#define SIG_IGN   1
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2
#define sigmask(s) (1UL << (s))

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

/* --- signals (Phase 20-G) --- */
/* The kernel returns here after a handler: the trampoline invokes sigreturn,
 * which restores the interrupted context. Its address is handed to the kernel
 * as sa_restorer by usignal(), so the kernel never needs a user symbol. */
extern void __sigreturn_trampoline(void);

static inline long ukill(int pid, int sig) {
    return usyscall(SYS_KILL, pid, sig, 0);
}
static inline long uraise(int sig) { return ukill((int)ugetpid(), sig); }

/* Install `handler` (or SIG_DFL/SIG_IGN) for `sig`. */
static inline long usignal(int sig, void (*handler)(int)) {
    return usyscall(SYS_RT_SIGACTION, sig, (long)handler,
                    (long)&__sigreturn_trampoline);
}
/* 64-bit signal mask by value; *oldset (if non-NULL) gets the prior mask. */
static inline long usigprocmask(int how, unsigned long set, unsigned long* oldset) {
    return usyscall(SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset);
}

/* --- arch_prctl: thread-local-storage base (Phase 20-H) --- */
static inline long uarch_prctl(int code, unsigned long addr) {
    return usyscall(SYS_ARCH_PRCTL, code, (long)addr, 0);
}
static inline long uset_fs(void* tls) {
    return uarch_prctl(ARCH_SET_FS, (unsigned long)tls);
}

/* --- time + randomness (Phase 20-I) --- */
static inline long uclock_gettime(int clk, struct timespec* ts) {
    return usyscall(SYS_CLOCK_GETTIME, clk, (long)ts, 0);
}
static inline long ugettimeofday(struct timeval* tv) {
    return usyscall(SYS_GETTIMEOFDAY, (long)tv, 0, 0);
}
static inline long unanosleep(const struct timespec* req, struct timespec* rem) {
    return usyscall(SYS_NANOSLEEP, (long)req, (long)rem, 0);
}
static inline long ugetrandom(void* buf, unsigned long len, unsigned int flags) {
    return usyscall(SYS_GETRANDOM, (long)buf, (long)len, (long)flags);
}
static inline long usleep_ms(long ms) {
    struct timespec r = { ms / 1000, (ms % 1000) * 1000000L };
    return unanosleep(&r, 0);
}

/* --- metadata / listing / fcntl (Phase 20-J) --- */
static inline long ustat(const char* path, struct stat* st) {
    return usyscall(SYS_STAT, (long)path, (long)st, 0);
}
static inline long ufstat(int fd, struct stat* st) {
    return usyscall(SYS_FSTAT, fd, (long)st, 0);
}
static inline long ugetdents64(int fd, void* buf, unsigned long n) {
    return usyscall(SYS_GETDENTS64, fd, (long)buf, (long)n);
}
static inline long ufcntl(int fd, int cmd, long arg) {
    return usyscall(SYS_FCNTL, fd, cmd, arg);
}

/* --- pipes / dup (Phase 20-K) --- */
static inline long upipe(int fds[2]) {
    return usyscall(SYS_PIPE, (long)fds, 0, 0);
}
static inline long udup2(int oldfd, int newfd) {
    return usyscall(SYS_DUP2, oldfd, newfd, 0);
}

/* --- threads / futex (Phase 20-L) --- */
static inline long ufutex(volatile int* uaddr, int op, int val, void* timeout) {
    return usyscall4(SYS_FUTEX, (long)uaddr, op, val, (long)timeout);
}
static inline long uset_tid_address(int* tidptr) {
    return usyscall(SYS_SET_TID_ADDRESS, (long)tidptr, 0, 0);
}
static inline long uwritev(int fd, const struct iovec* iov, int n) {
    return usyscall(SYS_WRITEV, fd, (long)iov, n);
}
static inline long ureadv(int fd, const struct iovec* iov, int n) {
    return usyscall(SYS_READV, fd, (long)iov, n);
}
/* Spawn a thread running fn(arg) on stack_top (grows down), sharing memory and
 * fds. ctid is set to the tid now and cleared + futex-woken on exit (join).
 * Implemented in start.S. Returns the new tid, or < 0 on error. */
extern long __clone_thread(void (*fn)(void*), void* stack_top, void* arg,
                           int* ctid, unsigned long flags);
#endif
