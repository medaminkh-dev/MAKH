/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - proc/user.c
 * User process lifecycle (Phase 20-A): spawn from an ELF, exit, waitpid.
 *
 * A user process is an ordinary kernel thread that additionally owns an
 * address_space_t and runs in ring 3. It is preemptible: the per-process
 * kernel stack is the trap stack (TSS.rsp0, set on every context switch), and
 * context_switch loads the process's CR3. fork()/execve-replacing-self, cwd
 * and the interactive tty-read path are Phase 20-A-2.
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
#include <lib/string.h>
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

/* Load `path` and start it as a ring-3 process. Returns the new pid, -errno. */
int proc_spawn_user(const char* path) {
    vnode_t* vn = vfs_resolve(path);
    if (!vn) return -ENOENT;

    address_space_t* as = kcalloc(1, sizeof(address_space_t));
    if (!as) return -ENOMEM;
    if (vmspace_create(as) != 0) { kfree(as); return -ENOMEM; }

    uint64_t entry = 0;
    int rc = elf_load(vn, as, &entry);
    if (rc != 0) { vmspace_destroy(as); kfree(as); return rc; }

    /* User stack. */
    for (uint64_t va = USTACK_TOP - USTACK_SIZE; va < USTACK_TOP; va += 4096) {
        void* fp = pmm_alloc_page();
        if (!fp) { vmspace_destroy(as); kfree(as); return -ENOMEM; }
        memset(fp, 0, 4096);
        page_setref((uint64_t)(uintptr_t)fp, 1);
        vmspace_map(as, va, (uint64_t)(uintptr_t)fp, PAGE_WRITABLE | PAGE_NO_EXECUTE);
    }
    uint64_t ustack = (USTACK_TOP - 16) & ~0xFULL;
    /* Minimal SysV stack: argc = 0 (no argv/envp/auxv yet; that arrives with
     * musl in Phase 20). Written through the identity map of the top page. */
    uint64_t top_pg_phys = vmspace_phys(as, USTACK_TOP - 4096);
    if (top_pg_phys)
        *(volatile uint64_t*)(uintptr_t)(top_pg_phys + (ustack - (USTACK_TOP - 4096))) = 0;

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
    preempt_enable();
    return (int)t->pid;
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
