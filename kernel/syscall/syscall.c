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

/* From usermode.c: leave ring 3. */
void usermode_exit(long code) __attribute__((noreturn));

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

    KLOG_I("SYSCALL", "ring-3 syscall/sysret enabled (STAR=%p)\n",
           (void*)rdmsr(IA32_STAR));
}

/* -------------------------------------------------------------------------- */
/* Individual calls. `from_user` selects pointer validation.                  */
/* -------------------------------------------------------------------------- */

static int64_t do_write(uint64_t fd, uint64_t ubuf, uint64_t count, int from_user) {
    if (count == 0) return 0;

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
        if (fd == 1 || fd == 2) {               /* stdout / stderr -> console */
            for (uint64_t i = 0; i < chunk; i++) terminal_putchar(tmp[i]);
        } else {                                /* a real file descriptor */
            long w = vfs_fd_write((int)fd, tmp, chunk);
            if (w < 0) return done ? (int64_t)done : w;
            if (w == 0) break;
            done += (uint64_t)w;
            continue;
        }
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t do_read(uint64_t fd, uint64_t ubuf, uint64_t count, int from_user) {
    if (fd == 0) {                                 /* stdin -> controlling tty */
        if (count == 0) return 0;
        char tmp[256];
        size_t chunk = count < sizeof(tmp) ? count : sizeof(tmp);
        long got = tty_read_blocking(tmp, chunk);  /* blocks until a line/EOF */
        if (got <= 0) return got;                  /* 0 = EOF, <0 = -errno */
        if (from_user) {
            if (copy_to_user((void*)(uintptr_t)ubuf, tmp, (size_t)got) < 0) return -EFAULT;
        } else {
            memcpy((void*)(uintptr_t)ubuf, tmp, (size_t)got);
        }
        return got;
    }
    if (fd == 1 || fd == 2) return 0;              /* reading stdout/stderr = EOF */
    if (count == 0) return 0;
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
        if ((uint64_t)got < chunk) break;
    }
    return (int64_t)done;
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
/* Dispatch                                                                   */
/* -------------------------------------------------------------------------- */

static int64_t dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, int from_user) {
    switch (num) {
        case SYS_WRITE:        return do_write(a1, a2, a3, from_user);
        case SYS_READ:         return do_read(a1, a2, a3, from_user);
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
        case SYS_MAKH_GETTICKS:return (int64_t)timer_get_ticks();
        case SYS_EXIT:
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
        case SYS_EXECVE: rc = proc_execve(tf, tf->rdi, tf->rsi, tf->rdx); break;
        default:
            rc = dispatch(tf->int_no, tf->rdi, tf->rsi, tf->rdx, tf->r10,
                          /*from_user*/1);
    }
    tf->rax = (uint64_t)rc;
    /* Deliver a pending fatal signal (e.g. Ctrl+C) now, before returning to
     * ring 3. If one is pending this does not return. */
    signal_check_and_die();
    return (uint64_t)rc;
}

int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    return dispatch(num, a1, a2, a3, 0, /*from_user*/0);
}
