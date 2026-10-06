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
#include <drivers/timer.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <signal.h>

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
    if (fd == 0 || fd == 1 || fd == 2) return 0;   /* no console input yet (EOF) */
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

static int64_t do_open(uint64_t upath, uint64_t flags, int from_user) {
    char path[VFS_PATH_MAX];
    if (from_user) {
        /* Copy the path a byte at a time until NUL or the limit. */
        size_t i = 0;
        for (; i < sizeof(path) - 1; i++) {
            char c;
            if (copy_from_user(&c, (const void*)(uintptr_t)(upath + i), 1) < 0) return -EFAULT;
            path[i] = c;
            if (!c) break;
        }
        path[i] = '\0';
    } else {
        size_t i = 0;
        const char* p = (const char*)(uintptr_t)upath;
        for (; i < sizeof(path) - 1 && p[i]; i++) path[i] = p[i];
        path[i] = '\0';
    }
    return vfs_open(path, (int)flags);
}

static int64_t do_getpid(void) {
    return current_process ? (int64_t)current_process->pid : 0;
}

/* -------------------------------------------------------------------------- */
/* Dispatch                                                                   */
/* -------------------------------------------------------------------------- */

static int64_t dispatch(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                        int from_user) {
    switch (num) {
        case SYS_WRITE:        return do_write(a1, a2, a3, from_user);
        case SYS_READ:         return do_read(a1, a2, a3, from_user);
        case SYS_OPEN:         return do_open(a1, a2, from_user);
        case SYS_CLOSE:        return vfs_close((int)a1);
        case SYS_LSEEK:        return vfs_lseek((int)a1, (long)a2, (int)a3);
        case SYS_GETPID:       return do_getpid();
        case SYS_KILL:         return signal_kill((int)a1, (int)a2);
        case SYS_MAKH_GETTICKS:return (int64_t)timer_get_ticks();
        case SYS_EXIT:
            if (from_user) usermode_exit((long)a1);  /* does not return */
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
     * number is in int_no (see usermode.asm). */
    int64_t rc = dispatch(tf->int_no, tf->rdi, tf->rsi, tf->rdx, /*from_user*/1);
    tf->rax = (uint64_t)rc;
    return (uint64_t)rc;
}

int64_t syscall_handler(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    return dispatch(num, a1, a2, a3, /*from_user*/0);
}
