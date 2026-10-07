/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_uvm.c
 * Phase 20-B: anonymous memory (brk, mmap/munmap/mprotect). The engine is
 * tested directly on a throwaway address space with a strict page-conservation
 * oracle; the syscalls are then tested end to end through real ring-3 programs.
 */

#include <ktest.h>
#include <mm/uvm.h>
#include <mm/vmspace.h>
#include <mm/pmm.h>
#include <syscall.h>                 /* PROT_*, MAP_* */
#include <errno.h>
#include <proc_internal.h>
#include <signal.h>

/* ---- engine-level: operate on a bare address space ---------------------- */

KTEST(uvm, mmap_maps_zeroed_pages_then_munmap_frees) {
    size_t base = pmm_get_free_memory();
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t mmc = USER_MMAP_BASE;

    long a = uvm_mmap(&as, &mmc, 3 * 4096, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE);
    KASSERT_TEST(a > 0);
    for (int i = 0; i < 3; i++) {
        uint64_t ph = vmspace_phys(&as, (uint64_t)a + (uint64_t)i * 4096);
        KASSERT_TEST(ph != 0);                       /* mapped */
        uint8_t* p = (uint8_t*)(uintptr_t)ph;         /* identity-mapped frame */
        int nonzero = 0;
        for (int k = 0; k < 4096; k++) if (p[k]) { nonzero = 1; break; }
        KEXPECT_EQ(nonzero, 0);                       /* zero-filled */
    }

    KEXPECT_EQ(uvm_munmap(&as, (uint64_t)a, 3 * 4096), 0);
    for (int i = 0; i < 3; i++)
        KEXPECT_EQ(vmspace_phys(&as, (uint64_t)a + (uint64_t)i * 4096), 0ull);

    vmspace_destroy(&as);
    KEXPECT_EQ(pmm_get_free_memory(), base);          /* net-zero */
}

KTEST(uvm, brk_grows_and_shrinks_cleanly) {
    size_t base = pmm_get_free_memory();
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t brk = USER_HEAP_BASE;

    KEXPECT_EQ(uvm_brk(&as, &brk, USER_HEAP_BASE, 0), (long)USER_HEAP_BASE);
    long nb = uvm_brk(&as, &brk, USER_HEAP_BASE, USER_HEAP_BASE + 4 * 4096);
    KEXPECT_EQ(nb, (long)(USER_HEAP_BASE + 4 * 4096));
    for (int i = 0; i < 4; i++)
        KASSERT_TEST(vmspace_phys(&as, USER_HEAP_BASE + (uint64_t)i * 4096) != 0);

    uvm_brk(&as, &brk, USER_HEAP_BASE, USER_HEAP_BASE);   /* shrink to empty */
    for (int i = 0; i < 4; i++)
        KEXPECT_EQ(vmspace_phys(&as, USER_HEAP_BASE + (uint64_t)i * 4096), 0ull);

    vmspace_destroy(&as);
    KEXPECT_EQ(pmm_get_free_memory(), base);
}

KTEST(uvm, mmap_rejects_bad_requests) {
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t mmc = USER_MMAP_BASE;
    KEXPECT_EQ(uvm_mmap(&as, &mmc, 0, PROT_READ, MAP_ANONYMOUS), (long)-EINVAL);
    KEXPECT_EQ(uvm_mmap(&as, &mmc, 4096, PROT_READ, MAP_PRIVATE), (long)-EINVAL);
    vmspace_destroy(&as);
}

KTEST(uvm, mprotect_leaves_the_page_mapped) {
    size_t base = pmm_get_free_memory();
    address_space_t as;
    KASSERT_TEST(vmspace_create(&as) == 0);
    uint64_t mmc = USER_MMAP_BASE;
    long a = uvm_mmap(&as, &mmc, 4096, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE);
    KASSERT_TEST(a > 0);
    KEXPECT_EQ(uvm_mprotect(&as, (uint64_t)a, 4096, PROT_READ), 0);
    KEXPECT(vmspace_phys(&as, (uint64_t)a) != 0);     /* still mapped, now RO */
    KEXPECT_EQ(uvm_munmap(&as, (uint64_t)a, 4096), 0);
    vmspace_destroy(&as);
    KEXPECT_EQ(pmm_get_free_memory(), base);
}

/* ---- end-to-end: real ring-3 programs drive the syscalls ---------------- */

KTEST(uvm, user_mmap_roundtrip) {
    int pid = proc_spawn_user("/bin/vmtest");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 0);            /* vmtest returns 0 iff mmap+brk all held */
}

KTEST(uvm, user_mprotect_read_only_faults_on_write) {
    int pid = proc_spawn_user("/bin/mprotfault");
    KASSERT_TEST(pid > 0);
    int status = -1;
    sys_waitpid(pid, &status);
    KEXPECT_EQ(status, 128 + SIGSEGV);   /* write to RO page -> SIGSEGV (139) */
}
