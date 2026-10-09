# MakhOS Phase 21-D (F21): MAKH rebuilds its own userland on MAKH

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — **MAKH uses its own toolchain (make + tcc + the musl
sysroot) to rebuild a program it ships, and the rebuilt binary behaves
identically.** The self-hosting summit. **177 in-kernel tests.**
**Depends on:** 21, 21-A (tcc), 21-B (libc), 21-C (make)

---

## 1. Scope

The F21 arc put the three pieces of a toolchain on MAKH: a compiler (21-A), a C
library (21-B) and a build system (21-C). This brick closes the loop — MAKH
regenerates a piece of **its own shipped userland** on itself.

`/bin/muslhello` is MAKH's first real-libc program (`user/musl/hello.c`): a
single source that exercises the entire libc ABI end to end — crt startup, TLS
through `%fs` (a `__thread` variable), the heap, buffered stdio, argv/auxv — and
exits **42** iff every part worked. The shipped binary is cross-built on a host
with zig cc (a static-PIE). This brick rebuilds that same source **on MAKH**,
with `make` driving `tcc` against the `/usr` musl sysroot, into a static non-PIE
executable, and shows it runs identically.

That the rebuilt binary exits 42 is a strong proof: it means tcc's codegen,
musl's crt, MAKH's `arch_prctl` TLS, the heap syscalls and the stdio/`writev`
path are all correct enough to carry a real program MAKH itself depends on —
built by MAKH, for MAKH.

## 2. The project (`user/selfhost/`)

Just a `Makefile`: it compiles `muslhello.c` with tcc and links it with musl's
crt + `libc.a` + `libtcc1.a`. `TCC`/`SYSROOT` default to MAKH's paths but are
overridable, so the same build runs on a host.

There is deliberately **no second copy of the source** in the repo: the
`$(INITRD)` recipe copies MAKH's own `user/musl/hello.c` to
`/share/selfhost/muslhello.c`, so the thing MAKH rebuilds is literally its
shipped source — one source of truth, nothing to drift.

## 3. The test (`kernel/tests/test_f21.c`)

`f21.make_rebuilds_makh_userland_on_makh`:
1. run the shipped `/bin/muslhello` → assert exit 42 (the host-built baseline);
2. `make -C /share/selfhost` → tcc rebuilds it on MAKH → assert make exits 0;
3. run `/share/selfhost/muslhello` → assert exit 42 — same ABI, same result.

Both runs print the same `hello from musl libc on MAKH: … tls=7` line (differing
only in `argv[0]`, the path each was launched from).

## 4. Status

**177 in-kernel tests pass**; `make stress` (24×4) is clean.

## 5. Next (beyond the summit)

The further reach is **tcc compiling tcc on MAKH** — a compiler rebuilding
itself from the full tinycc source tree. That is a heavier lift (the whole
source staged, a much longer build); this brick first proves the toolchain and
the libc ABI are sound enough to regenerate real MAKH userland. With the F21
trilogy complete, the road ahead is broadening userspace (more syscalls and
programs) and, later, SMP.
