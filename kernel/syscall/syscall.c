/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - syscall.c
 * System-call dispatch for ring 3 (Phase 16).
 *
 * The `syscall` instruction enters syscall_entry (usermode.asm), which builds
 * a trapframe and calls syscall_dispatch() here. We read arguments from the
 * frame, validate any user pointers through copy_from_user/copy_to_user, run
 * the call, and return a value following the Linux convention: >= 0 on success,
 * -errno on failure.
 */

#include <syscall.h>
#include <arch/cpu.h>
#include <arch/usermode.h>
#include <errno.h>
#include <kernel.h>
#include <klog.h>
#include <vga.h>
#include <proc_internal.h>
#include <sched.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <signal.h>
#include <mm/uvm.h>
#include <tty.h>
#include <ktime.h>
#include <krandom.h>
#include <mm/kheap.h>
#include <futex.h>

/* From usermode.c: leave ring 3. */
void usermode_exit(long code) __attribute__((noreturn));

/* nanosleep's parking queue — initialised in syscall_init, never woken. */
static wait_queue_t sleep_wq;

/* -------------------------------------------------------------------------- */
/* MSR setup                                                                  */
/* -------------------------------------------------------------------------- */

void syscall_init(void) {
    /* EFER.SCE enables the syscall/sysret instructions. */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1);

    /*
     * STAR:
     *   [47:32] syscall base  = 0x08  -> CS 0x08 (ring 0), SS 0x10
     *   [63:48] sysret  base  = 0x10  -> SS 0x18|3, CS 0x20|3 (ring 3)
     * This is why the GDT places user DATA (0x18) directly below user CODE
     * (0x20). See arch/gdt.h.
     */
    wrmsr(IA32_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* Clear IF, DF and AC on entry: interrupts off until we choose to sti,
     * forward string direction, and AC off so a stray user access under SMAP
     * still traps until uaccess deliberately raises it. */
    wrmsr(IA32_FMASK, 0x200 | 0x400 | 0x40000);

    wq_init(&sleep_wq);                       /* nanosleep's parking queue */
    futex_init();                             /* futex hash buckets */

    KLOG_I("SYSCALL", "ring-3 syscall/sysret enabled (STAR=%p)\n",
           (void*)rdmsr(IA32_STAR));
}

/* -------------------------------------------------------------------------- */
/* Individual calls. `from_user` selects pointer validation.                  */
/* -------------------------------------------------------------------------- */

static int64_t do_write(uint64_t fd, uint64_t ubuf, uint64_t count, int from_user) {
    if (count == 0) return 0;
    /* An open description (a file, or a pipe dup2'd onto 0/1/2) takes priority;
     * otherwise 1/2 fall back to the console. This keeps the default stdout path
     * unchanged while letting a pipeline redirect it (Phase 20-K). */
    int backed = (vfs_file((int)fd) != NULL);

    char tmp[256];
    uint64_t done = 0;
    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        if (from_user) {
            if (copy_from_user(tmp, (const void*)(uintptr_t)(ubuf + done), chunk) < 0)
                return done ? (int64_t)done : -EFAULT;
        } else {
            memcpy(tmp, (const void*)(uintptr_t)(ubuf + done), chunk);
        }
        if (backed) {
            long w = vfs_fd_write((int)fd, tmp, chunk);
            if (w < 0) return done ? (int64_t)done : w;
            if (w == 0) break;
            done += (uint64_t)w;
            continue;
        }
        if (fd == 1 || fd == 2) {               /* stdout / stderr -> console */
            for (uint64_t i = 0; i < chunk; i++) terminal_putchar(tmp[i]);
            done += chunk;
            continue;
        }
        return done ? (int64_t)done : -EBADF;   /* fd with no backing */
    }
    return (int64_t)done;
}

static int64_t do_read(uint64_t fd, uint64_t ubuf, uint64_t count, int from_user) {
    if (count == 0) return 0;
    file_t* f = vfs_file((int)fd);
    if (!f) {                                      /* fall back: tty / EOF / EBADF */
        if (fd == 0) {
            char tmp[256];
            size_t chunk = count < sizeof(tmp) ? count : sizeof(tmp);
            long got = tty_read_blocking(tmp, chunk);   /* blocks until a line/EOF */
            if (got <= 0) return got;
            if (from_user) {
                if (copy_to_user((void*)(uintptr_t)ubuf, tmp, (size_t)got) < 0) return -EFAULT;
            } else {
                memcpy((void*)(uintptr_t)ubuf, tmp, (size_t)got);
            }
            return got;
        }
        if (fd == 1 || fd == 2) return 0;          /* reading stdout/stderr = EOF */
        return -EBADF;
    }
    char tmp[256];
    uint64_t done = 0;
    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        long got = vfs_fd_read((int)fd, tmp, chunk);
        if (got < 0) return done ? (int64_t)done : got;
        if (got == 0) break;                    /* EOF */
        if (from_user) {
            if (copy_to_user((void*)(uintptr_t)(ubuf + done), tmp, (size_t)got) < 0)
                return done ? (int64_t)done : -EFAULT;
        } else {
            memcpy((void*)(uintptr_t)(ubuf + done), tmp, (size_t)got);
        }
        done += (uint64_t)got;
        if ((uint64_t)got < chunk) break;       /* short read (pipe/EOF): stop */
    }
    return (int64_t)done;
}

/* scatter/gather I/O (Phase F20-a): walk the iovec array, reusing do_read/
 * do_write per entry. musl's buffered stdio flushes through writev. */
struct iovec_k { uint64_t base; uint64_t len; };

static int64_t do_writev(uint64_t fd, uint64_t uiov, uint64_t iovcnt, int from_user) {
    if ((int64_t)iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        struct iovec_k v;
        if (copy_from_user(&v, (const void*)(uintptr_t)(uiov + i * sizeof(v)),
                           sizeof(v)) < 0)
            return total ? total : -EFAULT;
        if (v.len == 0) continue;
        int64_t w = do_write(fd, v.base, v.len, from_user);
        if (w < 0) return total ? total : w;
        total += w;
        if ((uint64_t)w < v.len) break;          /* short write: stop */
    }
    return total;
}

static int64_t do_readv(uint64_t fd, uint64_t uiov, uint64_t iovcnt, int from_user) {
    if ((int64_t)iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        struct iovec_k v;
        if (copy_from_user(&v, (const void*)(uintptr_t)(uiov + i * sizeof(v)),
                           sizeof(v)) < 0)
            return total ? total : -EFAULT;
        if (v.len == 0) continue;
        int64_t r = do_read(fd, v.base, v.len, from_user);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < v.len) break;          /* short/EOF: stop */
    }
    return total;
}

/* Copy a NUL-terminated path from `upath` into dst[dstsz]. Returns 0, -EFAULT
 * on a bad user address, or -ENAMETOOLONG if it does not fit. */
static int copy_path(char* dst, size_t dstsz, uint64_t upath, int from_user) {
    if (from_user) {
        size_t i = 0;
        for (; i < dstsz; i++) {
            char c;
            if (copy_from_user(&c, (const void*)(uintptr_t)(upath + i), 1) < 0)
                return -EFAULT;
            dst[i] = c;
            if (!c) return 0;
        }
        return -ENAMETOOLONG;
    }
    const char* p = (const char*)(uintptr_t)upath;
    size_t i = 0;
    for (; i < dstsz && p[i]; i++) dst[i] = p[i];
    if (i >= dstsz) return -ENAMETOOLONG;
    dst[i] = '\0';
    return 0;
}

/* Resolve a user-supplied path to an absolute one, relative to the calling
 * process's cwd (Phase 20-C). Kernel/non-user callers pass the path straight
 * through (it is expected to be absolute). */
static int resolve_path(char* abs, size_t abssz, const char* raw) {
    process_t* cur = current_process;
    if (cur && cur->is_user)
        return path_canonicalize(cur->cwd[0] ? cur->cwd : "/", raw, abs, abssz);
    size_t i = 0;
    for (; i < abssz - 1 && raw[i]; i++) abs[i] = raw[i];
    abs[i] = '\0';
    return 0;
}

static int64_t do_open(uint64_t upath, uint64_t flags, int from_user) {
    char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
    int rc = copy_path(path, sizeof(path), upath, from_user);
    if (rc < 0) return rc;
    if (resolve_path(abs, sizeof(abs), path) != 0) return -ENAMETOOLONG;
    return vfs_open(abs, (int)flags);
}

static int64_t do_chdir(uint64_t upath, int from_user) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user) return -ENOSYS;
    char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
    int rc = copy_path(path, sizeof(path), upath, from_user);
    if (rc < 0) return rc;
    if (path_canonicalize(cur->cwd[0] ? cur->cwd : "/", path, abs, sizeof(abs)) != 0)
        return -ENAMETOOLONG;
    vnode_t* vn = vfs_resolve(abs);
    if (!vn) return -ENOENT;
    if (vn->type != VNODE_DIR) return -ENOTDIR;
    size_t i = 0;
    for (; abs[i] && i < sizeof(cur->cwd) - 1; i++) cur->cwd[i] = abs[i];
    cur->cwd[i] = '\0';
    return 0;
}

static int64_t do_getcwd(uint64_t ubuf, uint64_t size, int from_user) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user) return -ENOSYS;
    const char* c = cur->cwd[0] ? cur->cwd : "/";
    size_t n = 0;
    while (c[n]) n++;
    n++;                                        /* include the NUL */
    if (size < n) return -ERANGE;
    if (from_user) {
        if (copy_to_user((void*)(uintptr_t)ubuf, c, n) < 0) return -EFAULT;
    } else {
        char* d = (char*)(uintptr_t)ubuf;
        for (size_t i = 0; i < n; i++) d[i] = c[i];
    }
    return (int64_t)n;                          /* bytes written, incl. NUL */
}

static int64_t do_getpid(void) {
    return current_process ? (int64_t)current_process->pid : 0;
}

/* wait4(pid, status, options, rusage): reap a child. options/rusage ignored. */
static int64_t do_wait4(uint64_t pid, uint64_t ustatus, int from_user) {
    int status = 0;
    int r = sys_waitpid((int)pid, &status);
    if (r >= 0 && ustatus) {
        if (from_user) {
            if (copy_to_user((void*)(uintptr_t)ustatus, &status, sizeof(status)) < 0)
                return -EFAULT;
        } else {
            *(int*)(uintptr_t)ustatus = status;
        }
    }
    return r;
}

/* -------------------------------------------------------------------------- */
/* Anonymous memory (Phase 20-B): operate on the calling process's space.      */
/* -------------------------------------------------------------------------- */

static int64_t do_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags) {
    (void)addr;                                  /* hint ignored: we bump-allocate */
    process_t* p = current_process;
    if (!p || !p->is_user || !p->aspace) return -ENOSYS;
    return uvm_mmap((address_space_t*)p->aspace, &p->mmap_cur, len,
                    (int)prot, (int)flags);
}

static int64_t do_munmap(uint64_t addr, uint64_t len) {
    process_t* p = current_process;
    if (!p || !p->is_user || !p->aspace) return -ENOSYS;
    return uvm_munmap((address_space_t*)p->aspace, addr, len);
}

static int64_t do_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    process_t* p = current_process;
    if (!p || !p->is_user || !p->aspace) return -ENOSYS;
    return uvm_mprotect((address_space_t*)p->aspace, addr, len, (int)prot);
}

static int64_t do_brk(uint64_t newbrk) {
    process_t* p = current_process;
    if (!p || !p->is_user || !p->aspace) return -ENOSYS;
    return uvm_brk((address_space_t*)p->aspace, &p->brk_cur, p->brk_start, newbrk);
}

/* -------------------------------------------------------------------------- */
/* Job control (Phase 20-F). The process-group / session calls live in         */
/* signal.c (sys_setpgid/getpgid/setsid) — the dispatch wires straight to them  */
/* so there is a single implementation. Only the terminal ioctl is new here.    */
/* -------------------------------------------------------------------------- */

/* ioctl(fd, request, arg): only the controlling-terminal job-control requests
 * are supported. fd must be the terminal (0/1/2); anything else is -ENOTTY.
 * arg points at a pid_t (the foreground process group). */
static int64_t do_ioctl(uint64_t fd, uint64_t request, uint64_t arg, int from_user) {
    if (fd != 0 && fd != 1 && fd != 2) return -ENOTTY;
    switch (request) {
        case TIOCGPGRP: {
            uint32_t pgid = tty_get_foreground();
            if (from_user) {
                if (copy_to_user((void*)(uintptr_t)arg, &pgid, sizeof(pgid)) < 0)
                    return -EFAULT;
            } else {
                *(uint32_t*)(uintptr_t)arg = pgid;
            }
            return 0;
        }
        case TIOCSPGRP: {
            uint32_t pgid;
            if (from_user) {
                if (copy_from_user(&pgid, (const void*)(uintptr_t)arg, sizeof(pgid)) < 0)
                    return -EFAULT;
            } else {
                pgid = *(const uint32_t*)(uintptr_t)arg;
            }
            tty_set_foreground(pgid);
            return 0;
        }
        default:
            return -EINVAL;
    }
}

/* Signal syscalls (Phase 20-G). Our ABI is simplified: sigaction passes the
 * handler and the user trampoline (sa_restorer) directly in registers, and
 * sigprocmask passes the 64-bit mask by value (not a sigset_t pointer). */
static int64_t do_sigaction(uint64_t sig, uint64_t handler, uint64_t restorer) {
    return signal_sigaction((int)sig, handler, restorer);
}

static int64_t do_sigprocmask(uint64_t how, uint64_t set, uint64_t uoldset) {
    uint64_t old = 0;
    int rc = signal_procmask((int)how, set, &old);
    if (rc < 0) return rc;
    if (uoldset && copy_to_user((void*)(uintptr_t)uoldset, &old, 8) < 0) return -EFAULT;
    return 0;
}

/* File metadata / listing / fcntl (Phase 20-J). stat/fstat copy a fixed struct
 * out; getdents64 fills a kernel buffer then copies it; fcntl is thin. */
static int64_t do_stat(uint64_t upath, uint64_t ust, int from_user) {
    char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
    int rc = copy_path(path, sizeof(path), upath, from_user);
    if (rc < 0) return rc;
    if (resolve_path(abs, sizeof(abs), path) != 0) return -ENAMETOOLONG;
    struct stat st;
    rc = vfs_stat(abs, &st);
    if (rc < 0) return rc;
    if (copy_to_user((void*)(uintptr_t)ust, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

static int64_t do_fstat(uint64_t fd, uint64_t ust) {
    struct stat st;
    int rc = vfs_fstat((int)fd, &st);
    if (rc < 0) return rc;
    if (copy_to_user((void*)(uintptr_t)ust, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

static int64_t do_getdents64(uint64_t fd, uint64_t ubuf, uint64_t count) {
    size_t cap = count < 4096 ? (size_t)count : 4096;
    if (cap == 0) return 0;
    void* kbuf = kmalloc(cap);
    if (!kbuf) return -ENOMEM;
    long r = vfs_getdents((int)fd, kbuf, cap);
    if (r > 0 && copy_to_user((void*)(uintptr_t)ubuf, kbuf, (size_t)r) < 0) r = -EFAULT;
    kfree(kbuf);
    return r;
}

static int64_t do_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg) {
    return vfs_fcntl((int)fd, (int)cmd, (long)arg);
}

/* pipe/pipe2: create a pipe, return the two fds to the user. (pipe2 flags are
 * accepted and ignored — no O_CLOEXEC/O_NONBLOCK tracking yet.) */
static int64_t do_pipe(uint64_t ufds, uint64_t flags) {
    (void)flags;
    int fds[2];
    int rc = vfs_pipe(fds);
    if (rc < 0) return rc;
    if (copy_to_user((void*)(uintptr_t)ufds, fds, sizeof(fds)) < 0) {
        vfs_close(fds[0]); vfs_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int64_t do_dup2(uint64_t oldfd, uint64_t newfd) {
    return vfs_dup2((int)oldfd, (int)newfd);
}

/* Thread primitives (Phase 20-L). */
static int64_t do_set_tid_address(uint64_t tidptr) {
    process_t* cur = current_process;
    if (!cur) return -ENOSYS;
    cur->clear_child_tid = tidptr;
    return (int64_t)cur->pid;
}

static int64_t do_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t utimeout) {
    int cmd = (int)op & FUTEX_CMD_MASK;             /* strip PRIVATE/CLOCK flags */
    if (cmd == FUTEX_WAIT) {
        uint64_t ticks = 0;
        if (utimeout) {
            struct timespec ts;
            if (copy_from_user(&ts, (const void*)(uintptr_t)utimeout, sizeof(ts)) < 0)
                return -EFAULT;
            uint64_t ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
            ticks = (ms * TIMER_FREQUENCY + 999) / 1000;
            if (ticks == 0) ticks = 1;              /* a tiny timeout still waits a tick */
        }
        return futex_wait(uaddr, (uint32_t)val, ticks);
    }
    if (cmd == FUTEX_WAKE) {
        return futex_wake(uaddr, (int)val);
    }
    return -ENOSYS;                                 /* other futex ops unsupported */
}

/* Time + randomness (Phase 20-I). nanosleep parks on a wait queue nothing ever
 * wakes, so only its own timeout or a signal ends it (interruptible -> -EINTR).
 * (sleep_wq is declared near the top so syscall_init can wq_init it.) */

static int64_t do_clock_gettime(uint64_t clk, uint64_t uts) {
    struct timespec ts;
    if (clock_gettime((int)clk, &ts) != 0) return -EINVAL;
    if (copy_to_user((void*)(uintptr_t)uts, &ts, sizeof(ts)) < 0) return -EFAULT;
    return 0;
}

static int64_t do_gettimeofday(uint64_t utv, uint64_t utz) {
    (void)utz;                                   /* obsolete timezone arg */
    uint64_t ms = clock_now_realtime_ms();       /* wall clock (RTC-anchored) */
    struct { int64_t tv_sec, tv_usec; } tv = {
        (int64_t)(ms / 1000), (int64_t)((ms % 1000) * 1000)
    };
    if (utv && copy_to_user((void*)(uintptr_t)utv, &tv, sizeof(tv)) < 0) return -EFAULT;
    return 0;
}

static int64_t do_nanosleep(uint64_t ureq, uint64_t urem) {
    struct timespec req;
    if (copy_from_user(&req, (const void*)(uintptr_t)ureq, sizeof(req)) < 0) return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) return -EINVAL;
    uint64_t req_ms = (uint64_t)req.tv_sec * 1000 + (uint64_t)req.tv_nsec / 1000000;
    if (req_ms == 0) return 0;
    uint64_t ticks = (req_ms * TIMER_FREQUENCY + 999) / 1000;   /* ceil to ticks */
    if (ticks == 0) ticks = 1;

    uint64_t start = clock_now_ms();
    irqflags_t f = local_irq_save();
    int rc = sched_wait_event(&sleep_wq, ticks, f);             /* restores f */
    if (rc == 2) {                                              /* signal woke us */
        uint64_t slept = clock_now_ms() - start;
        uint64_t rem = req_ms > slept ? req_ms - slept : 0;
        if (urem) {
            struct timespec r = { (int64_t)(rem / 1000),
                                  (int64_t)((rem % 1000) * 1000000) };
            copy_to_user((void*)(uintptr_t)urem, &r, sizeof(r));   /* best effort */
        }
        return -EINTR;
    }
    return 0;
}

static int64_t do_getrandom(uint64_t ubuf, uint64_t len, uint64_t flags) {
    (void)flags;                                 /* GRND_* ignored: never blocks */
    uint8_t tmp[256];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = (len - done) < sizeof(tmp) ? (size_t)(len - done) : sizeof(tmp);
        krandom_bytes(tmp, chunk);
        if (copy_to_user((void*)(uintptr_t)(ubuf + done), tmp, chunk) < 0)
            return done ? (int64_t)done : -EFAULT;
        done += chunk;
    }
    return (int64_t)len;
}

/* arch_prctl(code, addr): set/get the FS base (the thread pointer for TLS).
 * Only FS is user-owned; GS belongs to the kernel (swapgs), so it is refused.
 * The base is stored in the PCB and re-pinned every context switch. */
static int64_t do_arch_prctl(uint64_t code, uint64_t addr) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user) return -ENOSYS;
    switch (code) {
        case ARCH_SET_FS:
            cur->fs_base = addr;
            wrmsr(IA32_FS_BASE, addr);          /* effective immediately */
            return 0;
        case ARCH_GET_FS:
            if (copy_to_user((void*)(uintptr_t)addr, &cur->fs_base, 8) < 0)
                return -EFAULT;
            return 0;
        default:
            return -EINVAL;                      /* SET_GS/GET_GS unsupported */
    }
}

/* -------------------------------------------------------------------------- */
/* Dispatch                                                                   */
/* -------------------------------------------------------------------------- */

static int64_t dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, int from_user) {
    switch (num) {
        case SYS_WRITE:        return do_write(a1, a2, a3, from_user);
        case SYS_READ:         return do_read(a1, a2, a3, from_user);
        case SYS_WRITEV:       return do_writev(a1, a2, a3, from_user);
        case SYS_READV:        return do_readv(a1, a2, a3, from_user);
        case SYS_MADVISE:      return 0;                 /* advisory: no-op */
        case SYS_OPEN:         return do_open(a1, a2, from_user);
        case SYS_CHDIR:        return do_chdir(a1, from_user);
        case SYS_GETCWD:       return do_getcwd(a1, a2, from_user);
        case SYS_CLOSE:        return vfs_close((int)a1);
        case SYS_LSEEK:        return vfs_lseek((int)a1, (long)a2, (int)a3);
        case SYS_MMAP:         return do_mmap(a1, a2, a3, a4);
        case SYS_MUNMAP:       return do_munmap(a1, a2);
        case SYS_MPROTECT:     return do_mprotect(a1, a2, a3);
        case SYS_BRK:          return do_brk(a1);
        case SYS_GETPID:       return do_getpid();
        case SYS_WAIT4:        return do_wait4(a1, a2, from_user);
        case SYS_KILL:         return signal_kill((int)a1, (int)a2);
        case SYS_IOCTL:        return do_ioctl(a1, a2, a3, from_user);
        case SYS_RT_SIGACTION:   return do_sigaction(a1, a2, a3);
        case SYS_RT_SIGPROCMASK: return do_sigprocmask(a1, a2, a3);
        case SYS_ARCH_PRCTL:     return do_arch_prctl(a1, a2);
        case SYS_CLOCK_GETTIME:  return do_clock_gettime(a1, a2);
        case SYS_GETTIMEOFDAY:   return do_gettimeofday(a1, a2);
        case SYS_NANOSLEEP:      return do_nanosleep(a1, a2);
        case SYS_GETRANDOM:      return do_getrandom(a1, a2, a3);
        case SYS_STAT:
        case SYS_LSTAT:          return do_stat(a1, a2, from_user);   /* no symlinks */
        case SYS_FSTAT:          return do_fstat(a1, a2);
        case SYS_GETDENTS64:     return do_getdents64(a1, a2, a3);
        case SYS_FCNTL:          return do_fcntl(a1, a2, a3);
        case SYS_PIPE:           return do_pipe(a1, 0);
        case SYS_PIPE2:          return do_pipe(a1, a2);
        case SYS_DUP2:           return do_dup2(a1, a2);
        case SYS_FUTEX:          return do_futex(a1, a2, a3, a4);
        case SYS_SET_TID_ADDRESS:return do_set_tid_address(a1);
        case SYS_SETPGID:      return sys_setpgid((int)a1, (int)a2);
        case SYS_GETPGID:      return sys_getpgid((int)a1);
        case SYS_GETPGRP:      return sys_getpgid(0);           /* caller's group */
        case SYS_SETSID:       return sys_setsid();
        case SYS_MAKH_GETTICKS:return (int64_t)timer_get_ticks();
        case SYS_EXIT:
        case SYS_EXIT_GROUP:                         /* no thread groups: same as exit */
            if (from_user) {
                if (current_process && current_process->is_user)
                    thread_exit((int)(a1 & 0xff));   /* real process: become a zombie */
                usermode_exit((long)a1);             /* Phase-16 run_user_program */
            }
            return 0;
        case SYS_MAKH_SLEEP_MS:
            /* Deliberately not supported from the non-preemptible ring-3 brick;
             * returns success as a no-op for getticks-style demos. */
            return 0;
        default:
            return -ENOSYS;
    }
}

uint64_t syscall_dispatch(trapframe_t* tf) {
    /* System V/Linux argument registers were saved in the trapframe; the
     * number is in int_no (see usermode.asm). The 4th argument is r10 (the
     * syscall ABI's replacement for rcx, which `syscall` clobbers).
     *
     * fork() and execve() are handled here, not in dispatch(), because they
     * need the live trapframe: fork copies it for the child, and execve
     * rewrites it so the return path lands in the new image. */
    int64_t rc;
    switch (tf->int_no) {
        case SYS_FORK:   rc = proc_fork(tf); break;
        case SYS_CLONE:  rc = proc_clone(tf); break;   /* needs the live trapframe */
        case SYS_EXECVE: rc = proc_execve(tf, tf->rdi, tf->rsi, tf->rdx); break;
        case SYS_RT_SIGRETURN: rc = signal_sigreturn(tf); break;  /* restores tf */
        default:
            rc = dispatch(tf->int_no, tf->rdi, tf->rsi, tf->rdx, tf->r10,
                          /*from_user*/1);
    }
    tf->rax = (uint64_t)rc;
    /* Return-to-ring-3 signal delivery: run a pending handler (rewriting tf to
     * enter it) or apply a default action (e.g. Ctrl+C terminates). Has the
     * trapframe, so unlike the IRQ path it can build a handler frame. If a
     * default-terminate signal is taken this does not return. */
    signal_deliver(tf);
    return (uint64_t)rc;
}

int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    return dispatch(num, a1, a2, a3, 0, /*from_user*/0);
}
