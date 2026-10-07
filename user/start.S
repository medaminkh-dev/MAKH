/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* _start: call umain(), then exit(its return value). */
.text
.global _start
_start:
    xor %rbp, %rbp
    mov (%rsp), %rdi        /* argc   (at the top of the SysV initial stack) */
    lea 8(%rsp), %rsi       /* argv   (&argv[0], a NULL-terminated vector)   */
    call umain              /* umain(argc, argv); umain(void) ignores them   */
    mov %eax, %edi          /* exit code = umain() return */
    mov $60, %eax           /* SYS_EXIT */
    syscall
1:  jmp 1b

/* Signal-return trampoline (Phase 20-G). The kernel sets a signal handler's
 * return address to this; when the handler returns, %rsp points at the saved
 * context, and rt_sigreturn restores it. Touch nothing before the syscall. */
.global __sigreturn_trampoline
__sigreturn_trampoline:
    mov $15, %eax           /* SYS_RT_SIGRETURN */
    syscall

/* long __clone_thread(fn=rdi, stack_top=rsi, arg=rdx, ctid=rcx, flags=r8)
 * Phase 20-L. Sets up the child stack with fn+arg, issues clone, and in the
 * child pops them and runs fn(arg) then exit(0) (which triggers CLEARTID). */
.global __clone_thread
__clone_thread:
    sub $16, %rsi           /* reserve two slots at the top of the child stack */
    mov %rdi, 0(%rsi)       /* [stack] = fn  */
    mov %rdx, 8(%rsi)       /* [stack+8] = arg */
    mov %r8, %rdi           /* clone arg1: flags           */
                            /* rsi already = child stack   */
    xor %edx, %edx          /* ptid = NULL                 */
    mov %rcx, %r10          /* ctid -> r10 (4th syscall arg) */
    xor %r8d, %r8d          /* tls = 0                     */
    mov $56, %eax           /* SYS_CLONE                   */
    syscall
    test %rax, %rax
    jz 2f
    ret                     /* parent: %rax = child tid    */
2:  pop %rax                /* child: fn  */
    pop %rdi                /* child: arg */
    call *%rax              /* fn(arg)    */
    mov $60, %eax           /* SYS_EXIT(0) -> CLEARTID fires */
    xor %edi, %edi
    syscall
