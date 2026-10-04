/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/**
 * MakhOS - test_user.c
 * Phase 16: ring-3 userspace. These run real machine code in ring 3 through
 * run_user_program() and check the syscall ABI and fault containment.
 *
 * The programs are tiny hand-assembled x86-64 stubs (position-independent:
 * they build their data on the user stack, so they work wherever they load).
 */

#include <ktest.h>
#include <arch/usermode.h>
#include <proc.h>

/* write(1, "hi\n", 3); exit(7);  -- builds "hi\n" on the stack */
static const unsigned char prog_write_exit[] = {
    0xB8, 0x68, 0x69, 0x0A, 0x00,       /* mov eax, 0x000A6968   ("hi\n")      */
    0x50,                               /* push rax                            */
    0x48, 0x89, 0xE6,                   /* mov rsi, rsp          (buf)         */
    0xBF, 0x01, 0x00, 0x00, 0x00,       /* mov edi, 1            (fd=stdout)   */
    0xBA, 0x03, 0x00, 0x00, 0x00,       /* mov edx, 3            (len)         */
    0xB8, 0x01, 0x00, 0x00, 0x00,       /* mov eax, 1            (SYS_WRITE)   */
    0x0F, 0x05,                         /* syscall                             */
    0xBF, 0x07, 0x00, 0x00, 0x00,       /* mov edi, 7            (exit code)   */
    0xB8, 0x3C, 0x00, 0x00, 0x00,       /* mov eax, 60           (SYS_EXIT)    */
    0x0F, 0x05,                         /* syscall                             */
};

/* xor eax,eax ; mov byte [rax], 1  -- write to address 0 -> #PF (vector 14) */
static const unsigned char prog_fault[] = {
    0x31, 0xC0,                         /* xor eax, eax                        */
    0xC6, 0x00, 0x01,                   /* mov byte [rax], 1                   */
};

/* eax = getpid(); exit(eax);  -- returns the pid as the exit code */
static const unsigned char prog_getpid_exit[] = {
    0xB8, 0x27, 0x00, 0x00, 0x00,       /* mov eax, 39           (SYS_GETPID)  */
    0x0F, 0x05,                         /* syscall                             */
    0x89, 0xC7,                         /* mov edi, eax                        */
    0xB8, 0x3C, 0x00, 0x00, 0x00,       /* mov eax, 60           (SYS_EXIT)    */
    0x0F, 0x05,                         /* syscall                             */
};

KTEST(user, write_then_exit) {
    long status = -1;
    user_stop_t r = run_user_program(prog_write_exit, sizeof(prog_write_exit), &status);
    KEXPECT_EQ(r, USER_EXITED);
    KEXPECT_EQ(status, 7);              /* exit(7) delivered through the ABI   */
}

KTEST(user, ring3_fault_is_contained) {
    long status = -1;
    user_stop_t r = run_user_program(prog_fault, sizeof(prog_fault), &status);
    KEXPECT_EQ(r, USER_FAULTED);
    KEXPECT_EQ(status, 14);            /* #PF, caught, kernel survives         */

    /* Prove the kernel is fully alive after the fault: run another program. */
    long status2 = -1;
    KEXPECT_EQ(run_user_program(prog_write_exit, sizeof(prog_write_exit), &status2),
               USER_EXITED);
    KEXPECT_EQ(status2, 7);
}

KTEST(user, getpid_syscall_returns_value) {
    long status = -1;
    user_stop_t r = run_user_program(prog_getpid_exit, sizeof(prog_getpid_exit), &status);
    KEXPECT_EQ(r, USER_EXITED);
    KEXPECT_EQ(status, (long)(proc_current()->pid & 0xFF));
}

