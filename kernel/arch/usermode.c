/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - arch/usermode.c
 * Per-CPU setup and the ring-3 run harness (Phase 16).
 *
 * run_user_program() maps a user code page and stack, drops to ring 3, and
 * runs until the program calls exit() or faults. A fault in ring 3 is caught
 * by the exception handler and unwound here with longjmp, so a misbehaving
 * user program can never take down the kernel. ELF loading and real per-process
 * address spaces arrive in Phase 17; this is the trusted machinery underneath.
 */

#include <arch/usermode.h>
#include <arch/cpu.h>
#include <arch/tss.h>
#include <kfuzz.h>                 /* reused setjmp/longjmp */
#include <sched.h>
#include <proc_internal.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/kheap.h>
#include <lib/string.h>
#include <klog.h>
#include <kernel.h>

/* -------------------------------------------------------------------------- */
/* Per-CPU block (single CPU for now; SMP comes in Phase 22)                  */
/* -------------------------------------------------------------------------- */

static percpu_t boot_cpu;

void percpu_init(percpu_t* pc, uint32_t id) {
    pc->kernel_rsp = 0;
    pc->user_rsp_tmp = 0;
    pc->current = NULL;
    pc->id = id;
    /* Kernel GS base and the user-side KERNEL_GS_BASE both point here, so the
     * first swapgs on a syscall yields a valid pointer either way. */
    wrmsr(IA32_GS_BASE, (uint64_t)(uintptr_t)pc);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)pc);
}

percpu_t* this_cpu(void) { return &boot_cpu; }

void percpu_set_kernel_rsp(uint64_t rsp_top) { boot_cpu.kernel_rsp = rsp_top; }

/*
 * Prepare the CPU to run `next`. Called by schedule() just before the context
 * switch. It points TSS.rsp0 and the per-CPU syscall stack at the thread's own
 * kernel stack (so a ring-3 trap lands there), and re-pins the GS base to the
 * per-CPU block. Re-pinning every switch is the bulletproof single-CPU answer
 * to the fact that `mov gs` in context_switch wipes the active GS base: after
 * this, both GS bases are &boot_cpu, so any swapgs yields the per-CPU block.
 */
void arch_prepare_switch(struct process* next) {
    extern uint64_t proc_kstack_top(struct process*);   /* small accessor below */
    uint64_t top = proc_kstack_top(next);
    if (top) {
        tss_set_kernel_stack(top);
        boot_cpu.kernel_rsp = top;
    }
    wrmsr(IA32_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);
    /* Restore the thread-local-storage base (arch_prctl SET_FS, Phase 20-H).
     * FS is user-owned — the kernel never reads it — so this is the only place
     * it needs tending: 0 for a kernel thread, the process's TLS base otherwise.
     * context_switch saves/restores GP regs but not MSRs, so FS base lives in
     * the PCB and is re-pinned here, exactly like the TSS stack and GS base. */
    wrmsr(IA32_FS_BASE, next->fs_base);
    boot_cpu.current = next;
}

/*
 * Dedicated stack for handling traps that arrive from ring 3 (syscall entry,
 * and IRQ/#PF via TSS.rsp0). It MUST be separate from the kernel thread's own
 * C stack: run_user_program() is called from deep inside the kernel call chain
 * (kernel_main -> ktest -> ...), so routing ring-3 traps onto that same stack
 * at its top would overwrite the live frames we longjmp back into. A user
 * thread in the real process model is in ring 3 (its kernel stack empty) when
 * a trap arrives, so there this is simply its kernel stack; here we give the
 * nested user run its own.
 */
#define USER_KSTACK_SIZE 16384
static uint8_t* user_kstack;
static uint64_t user_kstack_top;

void usermode_init(void) {
    percpu_init(&boot_cpu, 0);
    user_kstack = kmalloc(USER_KSTACK_SIZE);
    if (!user_kstack) { KLOG_E("USER", "no memory for ring-3 trap stack\n"); return; }
    user_kstack_top = ((uint64_t)(uintptr_t)user_kstack + USER_KSTACK_SIZE) & ~0xFULL;
    KLOG_I("USER", "per-CPU block %p, ring-3 trap stack top %p\n",
           (void*)&boot_cpu, (void*)user_kstack_top);
}

/* -------------------------------------------------------------------------- */
/* Ring-3 run harness                                                         */
/* -------------------------------------------------------------------------- */

static kfuzz_jmp_buf   user_return;   /* where exit()/a fault unwinds to       */
static volatile int    in_user;       /* 1 while a user program is running     */
static volatile long   user_status;   /* exit code, or fault vector            */
static volatile user_stop_t user_stop;

int usermode_active(void) { return in_user; }

/* Called by sys_exit (kernel stack) to leave ring 3 for good. */
void usermode_exit(long code) {
    user_status = code;
    user_stop = USER_EXITED;
    kfuzz_longjmp(user_return, 1);
}

/* Called by the exception handler for a fault taken in ring 3. */
void usermode_fault(long vector) {
    user_status = vector;
    user_stop = USER_FAULTED;
    kfuzz_longjmp(user_return, 1);
}

/* Map one zeroed, user-accessible frame at virtual address `va`. `exec` picks
 * code (no NX, read-only to the kernel is not required here) vs. data (NX). */
static int map_user_frame(uint64_t va, int writable, int exec) {
    void* frame = pmm_alloc_page();
    if (!frame) return -1;
    uint64_t pf = (uint64_t)(uintptr_t)frame;
    memset(P2V(pf), 0, 4096);                       /* zero via HHDM (any CR3) */
    uint64_t flags = PAGE_PRESENT;
    if (writable) flags |= PAGE_WRITABLE;
    if (!exec) flags |= PAGE_NO_EXECUTE;        /* W^X: data/stack is non-exec */
    return vmm_map_user_page(va, pf, flags);
}

static uint64_t phys_of(uint64_t va) { return vmm_get_physical(va); }

user_stop_t run_user_program(const void* code, size_t len, long* status) {
    if (len == 0 || len > 4096) { if (status) *status = -1; return USER_FAULTED; }

    /* One code page + one stack page, both user-accessible. */
    if (map_user_frame(USER_CODE_BASE, /*w*/0, /*x*/1) != 0 ||
        map_user_frame(USER_STACK_TOP - 4096, /*w*/1, /*x*/0) != 0) {
        if (status) *status = -1;
        return USER_FAULTED;
    }
    /* Copy the program in through the kernel's identity mapping of the frame
     * (supervisor side), so SMAP never gets in the way of the load. */
    memcpy((void*)(uintptr_t)phys_of(USER_CODE_BASE), code, len);

    /* Ring-3 traps use the dedicated trap stack, never this thread's live C
     * stack (which holds the frames we longjmp back into). */
    percpu_set_kernel_rsp(user_kstack_top);
    tss_set_kernel_stack(user_kstack_top);

    /* Keep this thread on-CPU while it owns ring 3 (brick #1 is non-preemptive
     * in user mode; preemptible user threads come with the process model). */
    preempt_disable();
    in_user = 1;

    /*
     * Re-establish the per-CPU GS base immediately before dropping to ring 3.
     *
     * syscall_entry relies on `swapgs` loading IA32_KERNEL_GS_BASE to find the
     * kernel stack. But loading a segment SELECTOR resets that segment's base
     * to 0, and context_switch.asm reloads GS on every switch - so by the time
     * we re-enter ring 3 the per-CPU base has been wiped. Pinning both MSRs
     * here guarantees the next syscall's swapgs yields &boot_cpu. (A full fix -
     * reloading KERNEL_GS_BASE on every kernel entry - lands with SMP in Phase
     * 22; here the run is non-preemptible, so pinning once up front suffices.)
     */
    wrmsr(IA32_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&boot_cpu);

    if (kfuzz_setjmp(user_return) == 0) {
        enter_user_mode(USER_CODE_BASE, USER_STACK_TOP - 16);
        /* not reached */
    }

    /* Back in the kernel via usermode_exit() or usermode_fault(). */
    __asm__ volatile("mov $0x10, %%ax; mov %%ax, %%ds; mov %%ax, %%es" ::: "ax");
    in_user = 0;
    preempt_enable();

    uint64_t cp = phys_of(USER_CODE_BASE), sp = phys_of(USER_STACK_TOP - 4096);
    vmm_unmap_page(USER_CODE_BASE);
    vmm_unmap_page(USER_STACK_TOP - 4096);
    if (cp) pmm_free_page((void*)(uintptr_t)cp);
    if (sp) pmm_free_page((void*)(uintptr_t)sp);

    if (status) *status = user_status;
    return user_stop;
}
