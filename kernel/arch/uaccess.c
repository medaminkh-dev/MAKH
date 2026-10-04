/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - arch/uaccess.c
 * Safe access to user memory (Phase 16).
 *
 * A user pointer is never trusted. copy_from_user / copy_to_user run the copy
 * in __copy_user (usermode.asm); if the user address faults, the page-fault
 * handler consults uaccess_fixup() and resumes the copy at its fault label,
 * which returns the number of bytes left uncopied. A nonzero remainder becomes
 * -EFAULT. This is the fixup-table trick the Linux kernel uses, in miniature.
 */

#include <arch/usermode.h>
#include <errno.h>

/* The single uaccess copy site and its recovery point (usermode.asm). */
extern uint8_t __copy_user_fault_ip[];
extern uint8_t __copy_user_fault[];
extern uint64_t __copy_user(void* dst, const void* src, uint64_t n);

/*
 * uaccess_fixup - called from the page-fault handler. If the faulting RIP is
 * the uaccess copy instruction, return the address to resume at (the fault
 * label, which cleans up and returns the remaining count). Otherwise 0.
 *
 * One entry is enough today because all user copies funnel through the single
 * byte-copy loop; a table generalises this when more uaccess sites appear.
 */
uint64_t uaccess_fixup(uint64_t fault_rip) {
    if (fault_rip == (uint64_t)(uintptr_t)__copy_user_fault_ip)
        return (uint64_t)(uintptr_t)__copy_user_fault;
    return 0;
}

/* A user pointer must lie entirely in the canonical lower half and not wrap. */
int user_range_ok(const void* user_ptr, size_t n) {
    uint64_t a = (uint64_t)(uintptr_t)user_ptr;
    uint64_t end;
    if (__builtin_add_overflow(a, n, &end)) return 0;
    return end <= 0x0000800000000000ULL;      /* below the non-canonical gap */
}

long copy_from_user(void* dst, const void* user_src, size_t n) {
    if (!user_range_ok(user_src, n)) return -EFAULT;
    uint64_t left = __copy_user(dst, user_src, n);
    return left ? -EFAULT : (long)n;
}

long copy_to_user(void* user_dst, const void* src, size_t n) {
    if (!user_range_ok(user_dst, n)) return -EFAULT;
    uint64_t left = __copy_user(user_dst, src, n);
    return left ? -EFAULT : (long)n;
}
