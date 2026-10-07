/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* _start: call umain(), then exit(its return value). */
.text
.global _start
_start:
    xor %rbp, %rbp
    call umain
    mov %eax, %edi          /* exit code = umain() return */
    mov $60, %eax           /* SYS_EXIT */
    syscall
1:  jmp 1b
