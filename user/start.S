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
