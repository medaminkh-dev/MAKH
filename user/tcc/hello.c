/* SPDX-License-Identifier: AGPL-3.0-only */
/* Copyright (C) 2026 Amine Khemissi */
/* A freestanding C program that tcc compiles WHILE RUNNING ON MAKH (F21,
 * self-hosting). No libc: its own _start and inline-syscall write/exit. tcc
 * links it as a standard non-PIE ET_EXEC at 0x400000, which the higher-half
 * kernel (Phase 21) loads and runs. The exit code 42 is what the KTEST checks. */
static long sysw(int fd, const char* b, long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"((long)fd), "S"(b), "d"(n)
                     : "rcx", "r11", "memory");
    return r;
}
static void syse(int code) {
    __asm__ volatile("syscall" :: "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    for (;;) { }
}
void _start(void) {
    sysw(1, "hello from tcc on MAKH\n", 23);
    syse(42);
}
