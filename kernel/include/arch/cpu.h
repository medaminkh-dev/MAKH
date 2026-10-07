/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - arch/cpu.h
 * Per-CPU data and model-specific-register helpers (Phase 16).
 *
 * The `syscall` instruction does NOT switch stacks: on entry we are still on
 * the user stack, in ring 0, with no trustworthy pointer except GS. So each
 * CPU keeps a small block reachable through the GS base. syscall_entry swaps
 * to it, reads the kernel stack top, and builds its frame there.
 */

#ifndef MAKHOS_ARCH_CPU_H
#define MAKHOS_ARCH_CPU_H

#include <types.h>

/* IA32 MSRs used by the syscall path. */
#define IA32_EFER           0xC0000080
#define IA32_STAR           0xC0000081
#define IA32_LSTAR          0xC0000082
#define IA32_FMASK          0xC0000084
#define IA32_FS_BASE        0xC0000100
#define IA32_GS_BASE        0xC0000101
#define IA32_KERNEL_GS_BASE 0xC0000102

/*
 * Per-CPU block. GS base points here while in the kernel. The field offsets
 * are mirrored by assembly in usermode.asm, so keep the first two in place.
 */
typedef struct percpu {
    uint64_t kernel_rsp;     /* off 0: kernel stack top for syscall entry   */
    uint64_t user_rsp_tmp;   /* off 8: scratch to stash user RSP on entry   */
    struct process* current; /* off 16: current thread (mirrors proc)        */
    uint32_t id;             /* logical CPU id                               */
} percpu_t;

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* Read the time-stamp counter (cycles since reset): a cheap high-rate entropy
 * and timing source. Not a wall clock. */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Set up this CPU's per-CPU block and point IA32_GS_BASE at it. The user-side
 * IA32_KERNEL_GS_BASE starts equal, so the first `swapgs` on syscall entry
 * still yields a valid pointer. */
void percpu_init(percpu_t* pc, uint32_t id);

/* The per-CPU block for the (single) current CPU. */
percpu_t* this_cpu(void);

/* Update the kernel stack top used by the next syscall from ring 3. Called
 * when entering user mode and on every context switch. */
void percpu_set_kernel_rsp(uint64_t rsp_top);

#endif /* MAKHOS_ARCH_CPU_H */
