# MakhOS Phase 21-C (F21): make drives a multi-file build on MAKH

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — **GNU make, running on MAKH, drives tcc across a
multi-file C project and MAKH runs the result.** The third self-hosting brick.
**176 in-kernel tests.**
**Depends on:** 21 (higher-half kernel), 21-A (tcc on MAKH), 21-B (libc compile)

---

## 1. Scope

21-A/21-B put a compiler and a C library on MAKH. This brick adds the thing that
turns a compiler into a *toolchain you can drive*: a build system. GNU make runs
on MAKH, reads a `Makefile`, works out the targets, and fork/execs `tcc` for each
— two objects compiled and linked into one executable, with no host in the loop.

The crucial new exercise is **a user program spawning user programs**: make
`fork`/`posix_spawn`s tcc repeatedly, waits on each, and reacts to its exit
status. That path (a libc program using `clone` + `execve` + `wait4` in anger)
had two latent kernel bugs this brick found and fixed (§4).

Deferred: rebuilding MAKH's own userland on itself — the summit (F21-d).

## 2. The compiler driver (`user/make/`)

`user/make/make` is GNU make 4.4.1, built as a static-PIE musl binary and
checked in as a prebuilt (GPL-3.0, a separate work; `user/make/README.md` has
the `./configure --host=…-musl CC=zcc` recipe — the same ziglang toolchain the
tcc/busybox prebuilts use). It ships as `/bin/make`.

`user/make/proj/` is the demo project — `main.c` + `greet.c` + `greet.h` + a
`Makefile` — staged at `/share/mkproj`. Each recipe is a plain absolute-path
`tcc` command with no shell metacharacters, so make runs them directly
(`fork`+`execve`, no `/bin/sh` needed). `TCC`/`SYSROOT` default to MAKH's paths
but are overridable, so the same project also builds on a host.

## 3. The build MAKH runs

```
make -C /share/mkproj
  → tcc -nostdinc -I/usr/include -c main.c  -o main.o
  → tcc -nostdinc -I/usr/include -c greet.c -o greet.o
  → tcc -nostdlib -static -L/usr/lib crt1.o crti.o main.o greet.o crtn.o \
        -lc libtcc1.a -o app
```

The KTEST `f21.make_builds_multifile_project_on_makh` runs that, asserts make
exits 0, then runs `/share/mkproj/app` (prints its line via musl, exits 42).

## 4. The kernel bugs it found

A libc program fork/exec/waiting on children hit two real bugs, both fixed here:

- **execve freed a shared address space (use-after-free → triple fault).** musl's
  `posix_spawn` — what make uses to launch tcc — `clone`s with `CLONE_VM`, so the
  child runs in the *parent's* address space (vfork semantics) until it execs.
  `execve` tore the old space down unconditionally, pulling make's live page
  tables out from under it; the next switch to make fetched kernel code from a
  freed-and-reused PML4 and triple-faulted. `execve` now honours the address
  space refcount — destroy on the last reference, otherwise just drop ours —
  exactly as process reap already does.

- **close(stdout) on an implicit console fd returned EBADF.** Spawned processes
  get fd 0/1/2 as the implicit console via the read/write fast path, with no open
  description behind them. `close(1)` on such an empty slot returned `EBADF`;
  GNU make's `close_stdout` atexit handler treats that as a fatal "write error"
  and exits non-zero — even after a perfect build. Closing a never-opened std fd
  now succeeds (it frees nothing), while any other empty slot is still `EBADF`.

## 5. Tests

**176 in-kernel tests pass**; `make stress` (24×4) is clean. The fix paths are
exercised directly: the build forks and execs tcc three times out of make's
address space, and make's own clean exit depends on the close(1) fix.

## 6. Next

F21-d, the summit: use the on-MAKH toolchain (tcc + make + the musl sysroot) to
rebuild a piece of MAKH's own userland on MAKH itself.
