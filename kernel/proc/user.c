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
#include <krandom.h>
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

#define U_ARGC_MAX   32              /* most argv entries we accept (kernel-stack bound) */
#define U_ARGSTORE   1024            /* total bytes of argv string data                 */

/* ELF auxiliary-vector types we supply. The first three are what a freestanding
 * crt needs; the rest a real static-PIE libc (musl) reads at startup (Phase
 * 20-O): AT_PHDR/PHENT/PHNUM let it find its own program headers (to self-relo-
 * cate and to locate PT_TLS), AT_BASE is the load bias (0 — there is no interp),
 * and AT_RANDOM points at 16 bytes of entropy for the stack canary. */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_RANDOM 25

/* Write `n` bytes into address space `as` at user virtual address `va`, through
 * the identity map of its frames (so `as` need not be the active CR3). Handles
 * a write that straddles a page boundary. */
static void poke(address_space_t* as, uint64_t va, const void* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    while (n) {
        uint64_t phys = vmspace_phys(as, va & ~0xFFFULL);
        uint64_t off  = va & 0xFFF;
        size_t   chunk = 4096 - (size_t)off;
        if (chunk > n) chunk = n;
        if (phys) memcpy((void*)(uintptr_t)(phys + off), s, chunk);
        va += chunk; s += chunk; n -= chunk;
    }
}
static void poke64(address_space_t* as, uint64_t va, uint64_t v) {
    poke(as, va, &v, 8);
}

/* Build the ring-3 image of `vn` in the (already created) space `as`: load the
 * ELF segments and map a zeroed user stack. Sets *entry; the caller then lays
 * out the initial stack with setup_user_stack(). Returns 0 or -errno. */
static int build_user_image(address_space_t* as, vnode_t* vn, uint64_t* entry,
                            elf_aux_t* aux) {
    int rc = elf_load(vn, as, entry, aux);
    if (rc != 0) return rc;

    for (uint64_t va = USTACK_TOP - USTACK_SIZE; va < USTACK_TOP; va += 4096) {
        void* fp = pmm_alloc_page();
        if (!fp) return -ENOMEM;
        memset(fp, 0, 4096);
        page_setref((uint64_t)(uintptr_t)fp, 1);
        vmspace_map(as, va, (uint64_t)(uintptr_t)fp, PAGE_WRITABLE | PAGE_NO_EXECUTE);
    }
    return 0;
}

/* Lay out the SysV x86-64 initial process stack in `as` and return the value
 * RSP must have at entry (16-byte aligned, pointing at argc):
 *
 *   [argc][argv[0..argc-1]][NULL][envp[0..envc-1]][NULL][auxv...][AT_NULL]
 *   ... then the argv/envp string bytes near the top of the stack.
 *
 * argv_k/envp_k are kernel pointers to NUL-terminated strings. */
#define U_NAUX  8   /* auxv entries written below, AT_NULL included */

static uint64_t setup_user_stack(address_space_t* as, int argc, char* const argv_k[],
                                 int envc, char* const envp_k[], uint64_t entry,
                                 const elf_aux_t* aux) {
    uint64_t sp = USTACK_TOP;
    uint64_t argv_va[U_ARGC_MAX], envp_va[U_ARGC_MAX];

    /* Both callers bound these (execve caps its copy loop, spawn passes <=1),
     * but clamp defensively so the va[] arrays can never be overrun. */
    if (argc > U_ARGC_MAX) argc = U_ARGC_MAX;
    if (envc > U_ARGC_MAX) envc = U_ARGC_MAX;

    /* 16 bytes of entropy near the top of the stack for AT_RANDOM — musl seeds
     * its stack canary and TLS pointer guard from here. */
    uint8_t rnd[16];
    krandom_bytes(rnd, sizeof(rnd));
    sp -= sizeof(rnd); poke(as, sp, rnd, sizeof(rnd));
    uint64_t rand_va = sp;

    for (int i = envc - 1; i >= 0; i--) {              /* env strings */
        size_t len = strlen(envp_k[i]) + 1;
        sp -= len; poke(as, sp, envp_k[i], len); envp_va[i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {              /* arg strings */
        size_t len = strlen(argv_k[i]) + 1;
        sp -= len; poke(as, sp, argv_k[i], len); argv_va[i] = sp;
    }
    sp &= ~0xFULL;                                     /* end of the string area */

    /* Reserve the pointer arrays + auxv below the strings and 16-align argc. */
    size_t slots = 1 + (size_t)(argc + 1) + (size_t)(envc + 1) + U_NAUX * 2;
    uint64_t rsp = (sp - slots * 8) & ~0xFULL;

    uint64_t p = rsp;
    poke64(as, p, (uint64_t)argc);              p += 8;
    for (int i = 0; i < argc; i++) { poke64(as, p, argv_va[i]); p += 8; }
    poke64(as, p, 0);                           p += 8;      /* argv NULL */
    for (int i = 0; i < envc; i++) { poke64(as, p, envp_va[i]); p += 8; }
    poke64(as, p, 0);                           p += 8;      /* envp NULL */

    /* auxv (U_NAUX pairs, AT_NULL last). AT_PHDR/PHENT/PHNUM/BASE come from the
     * loader; a C runtime that does not need them (our own crt) just ignores
     * entries it does not recognise. */
    uint64_t at_phdr  = aux ? aux->at_phdr  : 0;
    uint64_t at_phent = aux ? aux->at_phent : 0;
    uint64_t at_phnum = aux ? aux->at_phnum : 0;
    uint64_t at_base  = aux ? aux->at_base  : 0;
    poke64(as, p, AT_PHDR);   p += 8; poke64(as, p, at_phdr);  p += 8;
    poke64(as, p, AT_PHENT);  p += 8; poke64(as, p, at_phent); p += 8;
    poke64(as, p, AT_PHNUM);  p += 8; poke64(as, p, at_phnum); p += 8;
    poke64(as, p, AT_PAGESZ); p += 8; poke64(as, p, 4096);     p += 8;
    poke64(as, p, AT_BASE);   p += 8; poke64(as, p, at_base);  p += 8;
    poke64(as, p, AT_ENTRY);  p += 8; poke64(as, p, entry);    p += 8;
    poke64(as, p, AT_RANDOM); p += 8; poke64(as, p, rand_va);  p += 8;
    poke64(as, p, AT_NULL);   p += 8; poke64(as, p, 0);        p += 8;
    return rsp;
}

/* Load `path` and start it as a ring-3 process with the given argv (argv_k are
 * kernel pointers; argc entries). Returns the new pid, -errno. */
static int spawn_user_core(const char* path, int argc, char* const argv_k[]) {
    vnode_t* vn = vfs_resolve(path);
    if (!vn) return -ENOENT;

    address_space_t* as = kcalloc(1, sizeof(address_space_t));
    if (!as) return -ENOMEM;
    if (vmspace_create(as) != 0) { kfree(as); return -ENOMEM; }

    uint64_t entry = 0;
    elf_aux_t aux;
    int rc = build_user_image(as, vn, &entry, &aux);
    if (rc != 0) { vmspace_destroy(as); kfree(as); return rc; }
    uint64_t ustack = setup_user_stack(as, argc, argv_k, 0, NULL, entry, &aux);

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

/* Spawn `path` with its own path as argv[0] (argc == 1) — the SysV convention a
 * C runtime expects even with no arguments. */
int proc_spawn_user(const char* path) {
    char* argv0[1] = { (char*)path };
    return spawn_user_core(path, 1, argv0);
}

/* Spawn `path` with a caller-supplied, NULL-terminated argv (argv[0] included).
 * Lets the kernel launch e.g. `busybox sh -c '...'`. */
int proc_spawn_user_argv(const char* path, char* const argv[]) {
    int argc = 0;
    while (argv && argv[argc]) argc++;
    return spawn_user_core(path, argc, argv);
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

    /* 4. Inherit the parent's user-process state, including the open-file table
     *    (shared descriptions, refcounts bumped — Phase 20-K). Console I/O still
     *    falls back to the fd 0/1/2 fast path when a slot is empty. */
    vfs_fork_fds(c, parent);
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
    for (int i = 0; i < 32; i++) c->sig_handlers[i] = parent->sig_handlers[i];
    c->sig_restorer = parent->sig_restorer;   /* handlers survive fork (Phase 20-G) */

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
/* clone() — create a thread sharing the address space (Phase 20-L)            */
/* -------------------------------------------------------------------------- */

#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/* clone(flags, child_stack, ptid, ctid, tls). With CLONE_VM the new thread
 * shares the caller's address space (refcounted) and fd table, runs on
 * child_stack with its own TLS, and returns 0 from clone (via fork_child_entry)
 * while the caller gets the new tid. Without CLONE_VM this is fork(). */
long proc_clone(trapframe_t* tf) {
    process_t* parent = current_process;
    if (!parent || !parent->is_user || !parent->aspace) return -ENOSYS;

    uint64_t flags       = tf->rdi;
    uint64_t child_stack = tf->rsi;
    uint64_t ptid        = tf->rdx;
    uint64_t ctid        = tf->r10;
    uint64_t newtls      = tf->r8;

    if (!(flags & CLONE_VM) || child_stack == 0) return proc_fork(tf);  /* a process */

    address_space_t* as = (address_space_t*)parent->aspace;

    process_t* c = proc_table_alloc();
    if (!c) return -ENOMEM;
    memset(c, 0, sizeof(*c));
    void* stk = kmalloc(DEFAULT_THREAD_STACK);
    if (!stk) { proc_table_free(c); return -ENOMEM; }
    memset(stk, 0, DEFAULT_THREAD_STACK);
    *(volatile uint64_t*)stk = STACK_CANARY_MAGIC;
    uint32_t pid = pid_alloc();
    if (!pid) { kfree(stk); proc_table_free(c); return -ENOMEM; }

    c->pid = pid;
    c->state = PROC_EMBRYO;
    c->priority = PRIO_DEFAULT;
    c->kernel_stack = (uint64_t)stk;
    c->kernel_stack_size = DEFAULT_THREAD_STACK;
    c->stack_canary = 1;
    c->time_slice = c->ticks_left = SCHED_QUANTUM;
    c->creation_time = timer_get_ticks();
    for (int i = 0; i < 31 && parent->name[i]; i++) c->name[i] = parent->name[i];

    c->is_user = 1;
    c->aspace = as;                        /* SHARE the parent's space */
    as->refcount++;
    c->user_entry = parent->user_entry;
    c->user_stack = child_stack;
    c->brk_start = parent->brk_start;
    c->brk_cur   = parent->brk_cur;
    c->mmap_cur  = parent->mmap_cur;
    for (int i = 0; i < (int)sizeof(c->cwd); i++) c->cwd[i] = parent->cwd[i];
    c->pgid = parent->pgid;
    c->sid  = parent->sid;
    c->sig_blocked = parent->sig_blocked;
    c->sig_ignore  = parent->sig_ignore;
    for (int i = 0; i < 32; i++) c->sig_handlers[i] = parent->sig_handlers[i];
    c->sig_restorer = parent->sig_restorer;
    c->fs_base = (flags & CLONE_SETTLS) ? newtls : parent->fs_base;

    if (flags & CLONE_FILES) vfs_share_fds(c, parent);
    else                     vfs_fork_fds(c, parent);

    if (flags & CLONE_CHILD_CLEARTID) c->clear_child_tid = ctid;
    c->detached = 1;                       /* a thread is joined via the futex, */
                                           /* not waitpid: auto-reap on exit     */

    /* Child returns from clone with rax=0 on its own stack (fork_child_entry
     * iretq's the copied trapframe). */
    uint64_t top = (c->kernel_stack + c->kernel_stack_size) & ~0xFULL;
    trapframe_t* ctf = (trapframe_t*)(uintptr_t)(top - sizeof(trapframe_t));
    *ctf = *tf;
    ctf->rax = 0;
    ctf->rsp = child_stack;
    c->context.rsp = (uint64_t)(uintptr_t)ctf;
    c->context.rip = (uint64_t)(uintptr_t)fork_child_entry;
    c->context.rflags = 0x002;
    c->context.cr3 = as->pml4_phys;
    c->context.cs = 0x08;
    c->context.ds = c->context.es = c->context.fs = c->context.gs = c->context.ss = 0x10;

    /* The tid stores land in the shared space (the caller's live CR3). */
    if (flags & CLONE_PARENT_SETTID) { uint32_t t = pid; copy_to_user((void*)(uintptr_t)ptid, &t, 4); }
    if (flags & CLONE_CHILD_SETTID)  { uint32_t t = pid; copy_to_user((void*)(uintptr_t)ctid, &t, 4); }

    irqflags_t f = local_irq_save();
    c->parent_pid = parent->pid;
    all_list_add(c);
    proc_add_child(parent, c);
    sched_admit(c);
    local_irq_restore(f);

    return (long)pid;                       /* caller: the new tid */
}

/* -------------------------------------------------------------------------- */
/* execve() — replace the caller's image with a fresh program (Phase 20-A-2)   */
/* -------------------------------------------------------------------------- */

long proc_execve(trapframe_t* tf, uint64_t upath, uint64_t uargv, uint64_t uenvp) {
    process_t* cur = current_process;
    if (!cur || !cur->is_user || !cur->aspace) return -ENOSYS;

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

    /* Copy argv in from the CALLER's address space now, while it is still
     * mapped — the strings live in the old image we are about to tear down. */
    char  argstore[U_ARGSTORE];
    char* argv_k[U_ARGC_MAX];
    int   argc = 0;
    size_t used = 0;
    if (uargv) {
        while (argc < U_ARGC_MAX) {
            uint64_t ptr;
            if (copy_from_user(&ptr, (const void*)(uintptr_t)(uargv + (uint64_t)argc * 8),
                               8) < 0) return -EFAULT;
            if (!ptr) break;                        /* NULL terminates argv */
            argv_k[argc] = &argstore[used];
            for (size_t j = 0;; j++) {
                if (used >= sizeof(argstore)) return -E2BIG;
                char c;
                if (copy_from_user(&c, (const void*)(uintptr_t)(ptr + j), 1) < 0) return -EFAULT;
                argstore[used++] = c;
                if (!c) break;
            }
            argc++;
        }
    }

    /* Copy envp the same way (strings live in the soon-to-be-freed image). A
     * real libc shell/make passes the environment to the program it execs. */
    char  envstore[U_ARGSTORE];
    char* envp_k[U_ARGC_MAX];
    int   envc = 0;
    size_t eused = 0;
    if (uenvp) {
        while (envc < U_ARGC_MAX) {
            uint64_t ptr;
            if (copy_from_user(&ptr, (const void*)(uintptr_t)(uenvp + (uint64_t)envc * 8),
                               8) < 0) return -EFAULT;
            if (!ptr) break;                        /* NULL terminates envp */
            envp_k[envc] = &envstore[eused];
            for (size_t j = 0;; j++) {
                if (eused >= sizeof(envstore)) return -E2BIG;
                char c;
                if (copy_from_user(&c, (const void*)(uintptr_t)(ptr + j), 1) < 0) return -EFAULT;
                envstore[eused++] = c;
                if (!c) break;
            }
            envc++;
        }
    }

    vnode_t* vn = vfs_resolve(abs);
    if (!vn) return -ENOENT;

    /* Build the new image in a brand-new space first; only swap once it is
     * fully loaded, so a failed execve leaves the caller untouched. */
    address_space_t* nas = kcalloc(1, sizeof(*nas));
    if (!nas) return -ENOMEM;
    if (vmspace_create(nas) != 0) { kfree(nas); return -ENOMEM; }
    uint64_t entry = 0;
    elf_aux_t aux;
    int rc = build_user_image(nas, vn, &entry, &aux);
    if (rc != 0) { vmspace_destroy(nas); kfree(nas); return rc; }
    /* Lay out argc/argv/envp/auxv on the new stack (identity-map writes). */
    uint64_t ustack = setup_user_stack(nas, argc, argv_k, envc, envp_k, entry, &aux);

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
    signal_reset_handlers(cur);   /* caught handlers point into the old image */

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
