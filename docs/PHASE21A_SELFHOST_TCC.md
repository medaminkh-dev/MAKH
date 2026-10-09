# MakhOS Phase 21-A (F21): tcc self-hosts — a compiler runs on MAKH

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — **tcc runs on MAKH, compiles C to a native executable,
and MAKH runs the result.** The first self-hosting brick. **174 in-kernel
tests.**
**Depends on:** 21 (higher-half kernel — the loadable low half this needs)

---

## 1. Scope

Phase 21 freed the lower canonical half so a standard non-PIE `ET_EXEC` at
`0x400000` can load. This brick spends that: it puts a real C compiler —
[TinyCC](https://repo.or.cz/tinycc.git), built as a static-PIE musl binary —
onto MAKH, and shows MAKH compiling and running a program **with no host in the
loop**.

That is the whole point of F21: a system that can build software for itself.
tcc is the natural first compiler — small, with a built-in assembler and linker,
so one `tcc foo.c` does codegen → object → link internally and emits an ELF
directly; MAKH never has to provide an external `as`/`ld`.

Deferred: a *libc*-linked compile (musl + crt + `libtcc1.a` staged on the MAKH
filesystem), then `make`, then rebuilding MAKH's own userland — the rest of the
self-hosting road.

## 2. Why it works now (and didn't before)

tcc's default output is a non-PIE `ET_EXEC` in the small code model, fixed at
`0x400000`. Before Phase 21 that address was the shared kernel identity map, so
tcc's output could not be loaded even though tcc itself ran. The higher-half
migration made `0x400000` the process's own, so the output loads like any other
program. tcc's entire syscall footprint to compile — `mmap`/`munmap`,
`open`/`read`/`close`, `writev`, `unlink`, `set_tid_address`,
`sched_getaffinity`, `arch_prctl`, `exit_group` — was already implemented across
Phases 20-B/J/L/N/S, so tcc runs unmodified.

## 3. The compiler (`user/tcc/`)

`user/tcc/tcc` is the checked-in prebuilt (LGPL-2.1, a separate work; see
`user/tcc/README.md` for the exact `./configure --cc=zcc; make tcc; strip`
recipe — the same ziglang musl toolchain the busybox/musl prebuilts use). It
ships into the initrd as `/bin/tcc`. `user/tcc/hello.c` — a freestanding program
(its own `_start` + inline syscalls) — ships as `/share/tcc-hello.c`.

## 4. Bounding the loader (`proc/elf.c`)

Opening the load window to the whole lower half means an untrusted ELF can now
present many valid-looking low segments, so `elf_load` gained two work caps on
top of the per-segment `USER_SEG_MAX`: `ELF_MAX_PHNUM` (reject absurd
program-header counts) and `ELF_MAX_TOTAL` (cap the summed image). A real
executable has a handful of headers and is a few MiB; the caps keep the KFUZZ
`elf` target's malformed inputs from driving the loader into a large allocation
loop.

## 5. Tests (`kernel/tests/test_f21.c`)

- **`f21.tcc_compiles_and_runs_on_makh`** — MAKH spawns
  `tcc -nostdlib -static -o /tmp/tcc-out /share/tcc-hello.c`, asserts tcc exits
  0, then execs `/tmp/tcc-out` and asserts it prints `hello from tcc on MAKH`
  and exits 42. Both the compiler and its freshly built output run on MAKH.

A real tcc compile under TCG takes a few seconds, so the default test timeout
rose to 180 s (CI already allots 240 s; stress 180 s per run).

**174 in-kernel tests pass**; `make stress` is clean.

## 6. Next

Stage a libc for tcc's output (musl + crt + `libtcc1.a`) so ordinary C programs
— not just freestanding ones — compile on MAKH; then `make`, then rebuild MAKH's
userland on itself.
