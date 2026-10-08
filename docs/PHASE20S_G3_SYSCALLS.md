# MakhOS Phase 20-S (G3): syscall surface for a real toolchain

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a real musl program does file I/O on the on-disk ext2
filesystem, and `execve` carries the environment. **168 in-kernel tests.**
**Depends on:** 20-R (ext2 write), 20-A-2 (execve)

---

## 1. Scope

Toward F21 (a self-hosting toolchain), this brick closes the syscall gaps a real
compiler/`make`/shell lean on, and proves the whole stack — libc → syscalls →
VFS → ext2 → virtio-blk — with real programs.

- **`execve` passes `envp`.** It had been stubbed (`(void)uenvp`); now the
  environment is copied from the caller and laid on the new program's SysV
  stack, so a shell or `make` can hand variables to the programs it runs.
- **The `*at` family**, routed to the existing path-based calls: `openat`,
  `newfstatat`, `unlinkat`, `faccessat`, plus plain `unlink`/`access`. A modern
  libc issues `openat(AT_FDCWD, …)` for every `open`, so this is what lets
  arbitrary musl file I/O work regardless of which form the libc chose.
- **Deferred:** a real directory fd (relative-to-`dirfd` with a non-cwd fd),
  `rename`/`renameat` and `mkdir` on ext2 (need the ext2 directory-write side,
  G2-d), and `poll`/`select` (added when a program needs them).

## 2. `execve` envp (`proc/user.c`)

`proc_execve` now copies the `envp` vector the same way it already copied
`argv` — each string out of the caller's (soon-to-be-freed) image into a kernel
buffer — and passes `envc`/`envp_k` to `setup_user_stack`, which places them
between the `argv` NULL terminator and the auxiliary vector. musl's
`__libc_start_main` finds them there and sets `__environ`, so `getenv` works in
the new image.

## 3. The `*at` and path calls (`syscall/syscall.c`)

- **`openat`/`newfstatat`/`unlinkat`/`faccessat`** accept `AT_FDCWD` (and
  absolute paths) and forward to `do_open`/`do_stat`/`do_unlink`/`do_access`,
  which already canonicalise against the process cwd. A non-`AT_FDCWD` dirfd
  with a *relative* path returns `-ENOSYS` for now.
- **`unlink`** routes to `vfs_unlink` (works on tmpfs today; ext2 unlink is
  G2-d). **`access`** resolves the path and reports existence — MAKH is
  single-user with no permission bits, so a resolvable path is accessible.

## 4. Fuzzed

No new KFUZZ target: these are thin routers onto already-fuzzed paths
(`do_open`'s resolver is hit by the `path`/`vfs` targets). The new coverage is
the two end-to-end program tests below.

## 5. Programs + tests (`user/musl/`, `kernel/tests/test_g3.c`)

- **`g3.musl_file_io_on_ext2`** (`/bin/fio`) — a musl program opens
  `/mnt/g3.txt` on the on-disk ext2 with `O_CREAT|O_RDWR|O_TRUNC`, writes a
  string, closes, reopens read-only and reads it back; exits 42 iff the bytes
  round-tripped. This is the full chain: libc `open`/`write`/`read` → VFS →
  ext2 write → virtio-blk → disk, and back.
- **`g3.execve_passes_envp`** (`/bin/envtest`) — started with no environment,
  it re-execs itself with `MAKH_G3=42` in `envp`; the child reads it with
  `getenv` and exits 42.

**168 in-kernel tests pass**; `make stress` (serial) is clean.

## 6. Next

G2-d (ext2 `mkdir`/`unlink`/`rename`) unlocks `rename`/`renameat`/`mkdir` at the
syscall layer too, after which F21 (port a C compiler, assemble+link, `make`,
rebuild MAKH on itself) is the path to self-hosting.
