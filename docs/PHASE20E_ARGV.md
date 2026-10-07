# MakhOS Phase 20-E: `argv` / `envp` / `auxv` (the SysV initial stack)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `execve` hands a program its arguments on a proper SysV
x86-64 initial stack, the shell tokenises a typed line into `argv`, and
`umain(argc, argv)` sees them. 142 in-kernel tests
**Depends on:** Phase 20-A (process model, ELF load, user stack), Phase 20-A-2
(`fork`/`execve`/`wait4`), Phase 20-D (the shell)

---

## 1. Scope

Phase 20-D booted to a prompt, but the shell could only run a *bare command
path* — there was no way to pass arguments. This brick adds the one thing every
real userland stands on: the **SysV AMD64 initial process stack**. When a
program starts, `%rsp` points at `argc`, followed by the `argv` pointer vector,
the `envp` pointer vector, and the auxiliary vector — exactly what a C runtime
(crt0, and later musl) expects to find.

Concretely:

- **`execve(path, argv, envp)`** copies `argv` out of the caller's address space
  *before* tearing the old image down, then lays it out on the new stack;
- **`umain(int argc, char** argv)`** — `_start` reads `argc`/`argv` off the stack
  and passes them in, so user programs can finally see their arguments;
- **the shell** splits a typed line on whitespace into `argv[0..n]` and
  `execve`s it, so `` /bin/echo a b `` runs `echo` with three arguments.

**Deferred:** a real `envp` (the machinery is in place — `setup_user_stack`
already takes `envc`/`envp_k` and lays env strings down; `execve` just passes
`envc = 0` for now, since nothing sets environment variables yet), and the wider
auxv (only `AT_PAGESZ`, `AT_ENTRY`, `AT_NULL` are supplied — enough for a static
crt0; `AT_RANDOM`/`AT_PHDR`/`AT_SYSINFO_EHDR` arrive with the dynamic-loader /
musl brick).

---

## 2. The SysV initial stack (`proc/user.c`)

The whole brick is one static helper, `setup_user_stack()`, which lays out the
stack the ABI mandates and returns the `%rsp` a program must start with:

```
 high ┌─────────────────────────┐ USTACK_TOP
      │  "…/echo\0" "a\0" "b\0"  │  arg/env string bytes
      ├─────────────────────────┤  (16-aligned gap)
      │  AT_NULL, 0              │  ┐
      │  AT_ENTRY, entry        │  │ auxv (key,val pairs)
      │  AT_PAGESZ, 4096        │  ┘
      │  NULL                   │  envp terminator
      │  envp[0..envc-1]        │
      │  NULL                   │  argv terminator
      │  argv[0..argc-1]        │  pointers into the string area above
 rsp→ │  argc                   │  ← %rsp at entry, 16-byte aligned
      └─────────────────────────┘
```

Two subtleties worth recording, because both are easy to get wrong:

1. **Strings first, from the top down.** The argument/environment *bytes* are
   copied to the very top of the stack; the pointer arrays below them store the
   resulting addresses. This is the order the ABI describes and keeps the
   pointer vectors contiguous and `argc`-anchored.
2. **`%rsp` is 16-aligned at entry, pointing at `argc`.** We compute the base as
   `rsp = (sp - slots*8) & ~0xF`, where `slots` counts `argc`, the two
   NULL-terminated pointer vectors, and the three auxv pairs. Masking guarantees
   alignment regardless of how many slots there are; because masking only lowers
   `rsp`, the pointer region never runs up into the string area.

Writes go through a small `poke(as, va, src, n)` helper that walks the target
address space's frames via the identity map (so it works on an address space
that is **not** the live CR3 — critical for `execve`, which builds the new stack
before switching) and handles a write that straddles a page boundary.
`build_user_image()` now only loads the ELF and maps a zeroed stack; laying out
the stack contents is `setup_user_stack`'s job.

## 3. `execve` copies argv before the switch (`proc/user.c`)

The ordering bug this brick had to avoid: `argv` strings live in the *old*
image, which `execve` destroys. So `proc_execve` copies the whole vector into a
kernel-side `argstore` (bounded: `U_ARGC_MAX = 32` entries, `U_ARGSTORE = 1024`
bytes → `-E2BIG` past that) **while the caller's address space is still mapped**,
and only then builds the new image, lays out the stack from the kernel copy, and
switches CR3. A `argv` pointer that faults returns `-EFAULT`; the caller is left
untouched on every error path (the new space is built first and only swapped in
on success).

`proc_spawn_user()` (the kernel-launched path, e.g. the boot shell) passes
`argc = 0`, so a process with no arguments still gets a valid, aligned stack
with a well-formed `argc`/`argv[NULL]`/auxv.

## 4. `umain(argc, argv)` (`user/start.S`, `user/*.c`)

`_start` now reads the ABI stack and calls the program's entry point with
arguments:

```asm
    xor  %rbp, %rbp
    mov  (%rsp), %rdi       /* argc  */
    lea  8(%rsp), %rsi      /* argv  (&argv[0]) */
    call umain             /* umain(argc, argv) */
    mov  %eax, %edi
    mov  $60, %eax          /* SYS_EXIT(umain's return) */
    syscall
```

Programs that do not care about arguments keep declaring `umain(void)` — the
extra register arguments are simply ignored. `user/echo.c` declares
`umain(int argc, char** argv)`, prints `argv[1..]`, and returns `argc` (so a
test can read the count back out as the exit status).

## 5. The shell parses a line into argv (`user/sh.c`)

The Phase-20-D shell ran a bare path; now it tokenises the line on spaces into
`argv[0..15]` (`argv[0]` is the command) and `execve`s with that vector. So the
interactive loop is finally `` $ /bin/echo hello world `` → `echo` runs with
`argc == 3`.

## 6. Fuzzed

No new KFUZZ target: the stack layout is deterministic (no parsing of untrusted
input in the kernel — `argv` bytes are copied verbatim with hard length caps),
and the surfaces it rests on are already hammered — `elf` (segment bounds),
`uvm` (the mappings the stack frames come from), and `path` (the `execve` path
argument). The adversarial coverage here is the bounded copy itself: `-E2BIG`
past `U_ARGSTORE`, `-EFAULT` on a bad `argv` pointer, `-ENAMETOOLONG` on a long
path, all exercised on the error paths.

## 7. Tests (`kernel/tests/test_args.c`) + programs (`user/`)

- **`args.execve_passes_argv`** (`/bin/execargs`) — isolates the loader's stack
  build: `execargs` calls `execve("/bin/echo", {echo, x, y, z})`, becomes
  `echo`, and exits with `argc == 4`. A status of 4 proves the vector arrived
  intact across the CR3 switch.
- **`args.shell_runs_command_with_args`** (`/bin/sh`) — end to end: the typed
  line `` /bin/echo a b `` is tokenised by the shell into a 3-element `argv`,
  run, and the child exits with `argc == 3`.

**142 in-kernel tests pass** (13 533 checks), up from 140 and non-regressive;
`make stress` (serial) is clean.

## 8. Deferred to the musl / dynamic-loader brick

| Item | Why it waits |
|---|---|
| a real `envp` | `setup_user_stack` already lays env down; nothing *sets* env yet |
| wider auxv (`AT_RANDOM`, `AT_PHDR`, `AT_SYSINFO_EHDR`) | needed by a dynamic loader, not a static crt0 |
| `arch_prctl(SET_FS)` for TLS | musl's thread pointer — the next critical syscall |

With `argv` in place, the ABI a static C runtime expects is complete: a program
links, loads, and sees its arguments. The remaining gap-#1 items are **job
control** (per-command process groups + `tcsetpgrp`) and **user signal
handlers** (`sigaction`/`sigreturn`); musl itself then needs `arch_prctl` and the
wider syscall surface.
