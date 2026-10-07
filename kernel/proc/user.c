/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - proc/user.c
 * User process lifecycle: spawn from an ELF, fork, execve, exit, waitpid.
 *
 * A user process is an ordinary kernel thread that additionally owns an
 * address_space_t and runs in ring 3. It is preemptible: the per-process
 * kernel stack is the trap stack (TSS.rsp0, set on every context switch), and
 * context_switch loads the process's CR3.
 *   - Phase 20-A: spawn (ELF -> new address space), exit, waitpid.
 *   - Phase 20-A-2: fork (COW clone + a fabricated syscall-return frame so the
 *     child returns 0 in ring 3) and execve (replace the image in place).
 * The interactive keyboard->tty read path and argv/envp/auxv are still to come.
 */

#include <proc_internal.h>
#include <sched.h>
#include <signal.h>
#include <elf.h>
#include <fs/vfs.h>
#include <arch/usermode.h>
#include <mm/vmspace.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <mm/kheap.h>
#include <mm/uvm.h>
#include <lib/string.h>
#include <drivers/timer.h>
#include <errno.h>
#include <klog.h>

#define USTACK_TOP   0x0000200000800000ULL   /* within the per-process region */
#define USTACK_SIZE  (64 * 1024)

/* The generic thread trampoline calls this in ring 0 (CR3 already the user
 * space); it just drops to ring 3 at the ELF entry. */
static void user_trampoline(void* arg) {
    (void)arg;
    process_t* cur = current_process;
    enter_user_mode(cur->user_entry, cur->user_stack);   /* noreturn */
}

/* Build the ring-3 image of `vn` in the (already created) space `as`: load the
 * ELF segments and map a zeroed user stack with argc=0 on top. Sets *entry and
 * *ustack on success; on failure returns -errno and the caller tears `as` down.
 * Shared by proc_spawn_user() and execve(). */
static int build_user_image(address_space_t* as, vnode_t* vn,
                            uint64_t* entry, uint64_t* ustack) {
    int rc = elf_load(vn, as, entry);
    if (rc != 0) return rc;

    for (uint64_t va = USTACK_TOP - USTACK_SIZE; va < USTACK_TOP; va += 4096) {
        void* fp = pmm_alloc_page();
        if (!fp) return -ENOMEM;
        memset(fp, 0, 4096);
        page_setref((uint64_t)(uintptr_t)fp, 1);
        vmspace_map(as, va, (uint64_t)(uintptr_t)fp, PAGE_WRITABLE | PAGE_NO_EXECUTE);
    }
    uint64_t sp = (USTACK_TOP - 16) & ~0xFULL;
    /* Minimal SysV stack: argc = 0 (argv/envp/auxv arrive with musl). Written
     * through the identity map of the top page. */
    uint64_t top_pg = vmspace_phys(as, USTACK_TOP - 4096);
    if (top_pg)
        *(volatile uint64_t*)(uintptr_t)(top_pg + (sp - (USTACK_TOP - 4096))) = 0;
    *ustack = sp;
    return 0;
}

/* Load `path` and start it as a ring-3 process. Returns the new pid, -errno. */
int proc_spawn_user(const char* path) {
    vnode_t* vn = vfs_resolve(path);
    if (!vn) return -ENOENT;

    address_space_t* as = kcalloc(1, sizeof(address_space_t));
    if (!as) return -ENOMEM;
    if (vmspace_create(as) != 0) { kfree(as); return -ENOMEM; }

    uint64_t entry = 0, ustack = 0;
    int rc = build_user_image(as, vn, &entry, &ustack);
    if (rc != 0) { vmspace_destroy(as); kfree(as); return rc; }

    /* thread_create() returns the thread already READY and queued, so a timer
     * tick could run user_trampoline before the user fields below are set —
     * dropping to ring 3 at entry 0 with the kernel CR3 still loaded. Hold off
     * preemption across creation and initialisation so the child cannot be
     * scheduled until it is fully formed (the same guard pthread_create uses).
     * Found by the stress run: a 1-in-8 #PF at RIP=0 in spawn_wait_many. */
    preempt_disable();
    process_t* t = thread_create(user_trampoline, NULL, "user", PRIO_DEFAULT);
    if (!t) { preempt_enable(); vmspace_destroy(as); kfree(as); return -ENOMEM; }

    t->is_user = 1;
    t->aspace = as;
    t->user_entry = entry;
    t->user_stack = ustack;
    t->context.cr3 = as->pml4_phys;      /* run in its own address space */
    t->brk_start = t->brk_cur = USER_HEAP_BASE;   /* Phase 20-B: empty heap */
    t->mmap_cur  = USER_MMAP_BASE;                /* mmap arena bump pointer */
    t->cwd[0] = '/'; t->cwd[1] = '\0';            /* Phase 20-C: start at root */
    preempt_enable();
    return (int)t->pid;
}

/* -------------------------------------------------------------------------- */
/* fork() — duplicate the caller into a new, COW address space (Phase 20-A-2)  */
/* -------------------------------------------------------------------------- */

/* The child resumes here when first scheduled: context_switch iretq's to
 * fork_child_entry (usermode_asm.asm), which repairs the GS base and falls into
 * the shared syscall-return epilogue with RSP pointing at the copy of the
 * parent trapframe we built — so it sysrets to ring 3 at the same instruction
 * as the parent, with rax = 0. */
extern void fork_child_entry(void);

long proc_fork(trapframe_t* tf) {
    process_t* parent = current_process;
    if (!parent || !parent->is_user || !parent->aspace) return -ENOSYS;

    /* 1. Child address space: a copy-on-write clone of the parent's. */
    address_space_t* cas = kcalloc(1, sizeof(*cas));
    if (!cas) return -ENOMEM;
    if (vmspace_fork((address_space_t*)parent->aspace, cas) != 0) {
        kfree(cas); return -ENOMEM;
    }

    /* 2. Child PCB + kernel stack. */
    process_t* c = proc_table_alloc();
    if (!c) { vmspace_destroy(cas); kfree(cas); return -ENOMEM; }
    memset(c, 0, sizeof(*c));
    void* stk = kmalloc(DEFAULT_THREAD_STACK);
    if (!stk) { proc_table_free(c); vmspace_destroy(cas); kfree(cas); return -ENOMEM; }
    memset(stk, 0, DEFAULT_THREAD_STACK);
    *(volatile uint64_t*)stk = STACK_CANARY_MAGIC;      /* overflow guard */
    uint32_t pid = pid_alloc();
    if (!pid) { kfree(stk); proc_table_free(c); vmspace_destroy(cas); kfree(cas); return -ENOMEM; }

    /* 3. Scheduler bookkeeping. */
    c->pid = pid;
    c->state = PROC_EMBRYO;
    c->priority = PRIO_DEFAULT;
    c->kernel_stack = (uint64_t)stk;
    c->kernel_stack_size = DEFAULT_THREAD_STACK;
    c->stack_canary = 1;
    c->time_slice = c->ticks_left = SCHED_QUANTUM;
    c->creation_time = timer_get_ticks();
    for (int i = 0; i < 31 && parent->name[i]; i++) c->name[i] = parent->name[i];

    /* 4. Inherit the parent's user-process state. (Open fds are NOT inherited
     *    in this first brick — console I/O uses the fd 0/1/2 fast path, so a
     *    forked child still has stdin/out/err; real fd inheritance lands with
     *    the file refcount work alongside pipe/dup.) */
    c->is_user = 1;
    c->aspace = cas;
    c->user_entry = parent->user_entry;
    c->user_stack = parent->user_stack;
    c->brk_start = parent->brk_start;
    c->brk_cur   = parent->brk_cur;
    c->mmap_cur  = parent->mmap_cur;
    for (int i = 0; i < (int)sizeof(c->cwd); i++) c->cwd[i] = parent->cwd[i];
    c->pgid = parent->pgid;
    c->sid  = parent->sid;
    c->sig_blocked = parent->sig_blocked;
    c->sig_ignore  = parent->sig_ignore;

    /* 5. Fork-return context: a copy of the parent's trapframe, rax = 0, with
     *    the saved context pointed at the syscall-return epilogue. */
    uint64_t top = (c->kernel_stack + c->kernel_stack_size) & ~0xFULL;
    trapframe_t* ctf = (trapframe_t*)(uintptr_t)(top - sizeof(trapframe_t));
    *ctf = *tf;
    ctf->rax = 0;                                       /* child: fork() == 0 */
    c->context.rsp = (uint64_t)(uintptr_t)ctf;
    c->context.rip = (uint64_t)(uintptr_t)fork_child_entry;
    c->context.rflags = 0x002;                          /* IF=0 until sysret */
    c->context.cr3 = cas->pml4_phys;
    c->context.cs = 0x08;
    c->context.ds = c->context.es = c->context.fs = c->context.gs = c->context.ss = 0x10;

    /* 6. Link into the shared lists and admit to the run queue (IRQs off: the
     *    lists must not be seen mid-update by a preemption; admit is last, so
     *    the child cannot run before it is fully formed). */
    irqflags_t f = local_irq_save();
    c->parent_pid = parent->pid;
    all_list_add(c);
    proc_add_child(parent, c);
    sched_admit(c);
    local_irq_restore(f);

    return (long)pid;                                   /* parent: child's pid */
}

/* -------------------------------------------------------------------------- */
/* execve() — replace the caller's image with a fresh program (Phase 20-A-2)   */
/* -------------------------------------------------------------------------- */

long proc_execve(trapframe_t* tf, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user || !cur->aspace) return -ENOSYS;
    (void)uargv; (void)uenvp;              /* argc=0 for this brick (no argv yet) */

    /* Copy and canonicalise the path against the process cwd. */
    char raw[256], abs[256];
    for (size_t i = 0;; i++) {
        if (i >= sizeof(raw)) return -ENAMETOOLONG;
        char ch;
        if (copy_from_user(&ch, (const void*)(uintptr_t)(upath + i), 1) < 0) return -EFAULT;
        raw[i] = ch;
        if (!ch) break;
    }
    if (path_canonicalize(cur->cwd[0] ? cur->cwd : "/", raw, abs, sizeof(abs)) != 0)
        return -ENAMETOOLONG;

    vnode_t* vn = vfs_resolve(abs);
    if (!vn) return -ENOENT;

    /* Build the new image in a brand-new space first; only swap once it is
     * fully loaded, so a failed execve leaves the caller untouched. */
    address_space_t* nas = kcalloc(1, sizeof(*nas));
    if (!nas) return -ENOMEM;
    if (vmspace_create(nas) != 0) { kfree(nas); return -ENOMEM; }
    uint64_t entry = 0, ustack = 0;
    int rc = build_user_image(nas, vn, &entry, &ustack);
    if (rc != 0) { vmspace_destroy(nas); kfree(nas); return rc; }

    /* Swap address spaces. We run on the shared kernel stack, so switching CR3
     * and freeing the old space is safe (the kernel half stays mapped). */
    address_space_t* old = (address_space_t*)cur->aspace;
    irqflags_t f = local_irq_save();
    cur->aspace = nas;
    cur->context.cr3 = nas->pml4_phys;
    vmspace_switch(nas);                    /* load the new CR3 now */
    local_irq_restore(f);
    vmspace_destroy(old);
    kfree(old);

    /* Fresh heap/mmap; cwd and pgid/sid are preserved across exec. */
    cur->brk_start = cur->brk_cur = USER_HEAP_BASE;
    cur->mmap_cur  = USER_MMAP_BASE;
    cur->user_entry = entry;
    cur->user_stack = ustack;

    /* Rewrite the trapframe so the syscall epilogue returns into the new image
     * with a clean register file (sysret takes rip from rcx and rflags from
     * r11, which the epilogue loads from tf->rip / tf->rflags). */
    tf->r15 = tf->r14 = tf->r13 = tf->r12 = tf->r11 = tf->r10 = tf->r9 = tf->r8 = 0;
    tf->rbp = tf->rdi = tf->rsi = tf->rdx = tf->rcx = tf->rbx = 0;
    tf->rax    = 0;
    tf->rip    = entry;
    tf->rsp    = ustack;
    tf->rflags = 0x202;                     /* IF=1 */
    return 0;                               /* new program runs on return */
}

/* Called from the exception handler when a running user process faults. The
 * process is terminated with 128 + SIGSEGV (POSIX convention). Never returns. */
void proc_user_fault(long vector) {
    KLOG_W("USER", "pid %u killed by SIGSEGV (vector %ld)\n",
           current_process ? current_process->pid : 0, vector);
    thread_exit(128 + SIGSEGV);
}

/* waitpid(-1): block until any child exits, reap it, return its pid; the exit
 * code goes to *status. Returns -ECHILD if there are no children. */
int sys_waitpid(int pid, int* status) {
    process_t* cur = current_process;
    for (;;) {
        irqflags_t f = local_irq_save();
        process_t* zombie = NULL;
        int have_children = 0;
        for (process_t* p = all_processes.head; p; p = p->all_next) {
            /* Only real user-process children are waited on here; kernel
             * worker threads (also parented to pid 1) are joined, not reaped
             * through waitpid, and must not keep this call from returning. */
            if (!p->is_user || p->parent_pid != cur->pid || p == cur) continue;
            if (pid > 0 && (int)p->pid != pid) continue;
            have_children = 1;
            if (p->state == PROC_ZOMBIE && !p->reaped) { zombie = p; break; }
        }
        if (zombie) {
            int code = zombie->exit_code;
            int cpid = (int)zombie->pid;
            local_irq_restore(f);
            if (status) *status = code;
            proc_reap(zombie);          /* frees aspace, stack, pid, table slot */
            return cpid;
        }
        if (!have_children) { local_irq_restore(f); return -ECHILD; }
        /* Children still alive: block until one exits (or a signal arrives). */
        sched_wait_event(&cur->child_wq, 0, f);
    }
}
