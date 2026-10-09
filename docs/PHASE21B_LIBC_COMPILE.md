# MakhOS Phase 21-B (F21): tcc links libc programs on MAKH

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — **tcc compiles a real `<stdio.h>`/`<stdlib.h>`/`<string.h>`
program against musl, on MAKH, and MAKH runs the result.** The second
self-hosting brick. **175 in-kernel tests.**
**Depends on:** 21 (higher-half kernel), 21-A (tcc runs on MAKH)

---

## 1. Scope

21-A proved tcc runs on MAKH and compiles a *freestanding* program (its own
`_start`, inline syscalls, `-nostdlib`). This brick removes the "freestanding"
caveat: it stages a **musl sysroot** on the MAKH filesystem — headers, a static
`libc.a`, the C-runtime objects (`crt1.o`/`crti.o`/`crtn.o`) and tcc's own
compiler-runtime `libtcc1.a` — so an ordinary C program that `#include`s the
standard headers and calls `printf`/`malloc`/`strcpy` compiles and links **on
MAKH**, then runs on MAKH.

That is the difference between "a compiler runs here" and "software is built
here": real programs link against a real C library, with no host in the loop.

Deferred: `make` driving a multi-file build, then rebuilding MAKH's own userland
on itself — the rest of the self-hosting road.

## 2. The sysroot (`user/sysroot/usr/`)

A conventional `/usr` tree, built once on the host from musl 1.2.6 with the same
ziglang musl toolchain the other prebuilts use, and checked in:

```
usr/include/   musl's headers (the full tree, 217 files)
usr/lib/libc.a         the static C library (stripped)
usr/lib/crt1.o         _start → __libc_start_main → main
usr/lib/crti.o         .init/.fini prologue
usr/lib/crtn.o         .init/.fini epilogue
usr/lib/libtcc1.a      tcc's compiler-runtime helpers (LGPL, with tcc)
```

The `$(INITRD)` recipe copies `user/sysroot/usr` into the archive root, so the
tree lands at `/usr` in the boot tmpfs. `user/tcc/full.c` — the libc test
program — ships as `/share/tcc-full.c`. Licensing is unchanged: musl is MIT, tcc
(and `libtcc1.a`) LGPL-2.1, both separate works from MAKH (AGPL); see
`user/sysroot/README.md`.

## 3. The compile MAKH runs

```
tcc -nostdinc -nostdlib -static -I/usr/include \
    /usr/lib/crt1.o /usr/lib/crti.o /share/tcc-full.c /usr/lib/crtn.o \
    -L/usr/lib -lc /usr/lib/libtcc1.a -o /tmp/full
```

`-nostdinc -nostdlib` hand tcc **only** the MAKH sysroot (no host path ever
leaks in); the crt objects and `-lc` bring up musl's `__libc_start_main` on
MAKH's syscall surface; `libtcc1.a` supplies the compiler-runtime helpers tcc
emits calls to. The output is a static, non-PIE `ET_EXEC` at `0x400000` — the
same low address 21-A made loadable — so the higher-half kernel loads it like
any other program. It runs, `printf`s through musl→`writev`, and exits 42.

## 4. Why it works now (the initrd-reservation fix)

Staging the sysroot pushed the initrd from ~1.8 MiB to ~4.3 MiB, which exposed a
latent physical-memory bug. GRUB loads the initrd module into free RAM wherever
it likes — here at physical `0x196000` — but the PMM only ever reserved a fixed
low window (the first 4 MiB) plus the kernel image. A module that extends past
that window sat on frames the allocator considered free, so the heap, the
`struct page` array and the tmpfs buffers `tar_load_initrd()` itself allocates
**overwrote the tail of the archive while it was still being read** — the parser
hit a corrupted header partway through `/usr` and silently stopped, so the
later files (`libtcc1.a`, `crt1.o`, all of `/usr/include`) never materialized.

The fix is a general one: `pmm_reserve_region()` marks an arbitrary physical
byte range as used, and `kernel_main` calls it for the initrd module's extent
**immediately after `pmm_init()`**, before any allocation can land inside it.
The small initrd never tripped this (it ended below the 4 MiB mark); any initrd
large enough to cross its load address + the low reservation would have. It is
fixed for good, independent of initrd size.

## 5. Tests (`kernel/tests/test_f21.c`)

- **`f21.tcc_compiles_libc_program_on_makh`** — MAKH spawns the §3 command line,
  asserts tcc exits 0 (compiled + linked against musl), then execs `/tmp/full`
  and asserts it prints `hello from a full libc program on MAKH, 42` and exits
  42. A full libc program is built and run entirely on MAKH.

**175 in-kernel tests pass**; `make stress` (24×4) is clean.

## 6. Next

`make` on MAKH: drive tcc across a small multi-file project from a Makefile,
then turn the toolchain on MAKH's own userland.
