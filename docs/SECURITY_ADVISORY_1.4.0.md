<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH Security Advisory — 1.4.0

**Affected:** MAKH 1.3.0 and earlier
**Fixed in:** 1.4.0
**Found by:** UFUZZ (the ring-3 syscall fuzzer built in 1.4.0) and the reviews
that came with its ABI contract

All issues below were found and fixed before 1.4.0 shipped. MAKH has no
multi-user accounts and no remote administration, so the realistic attacker is
a **local unprivileged program**; severity is rated in that model (P1 = kernel
compromise or reliable panic from unprivileged input; P2 = integrity or
correctness violation of a documented contract with narrower impact). Each fix
has a regression test that failed before the fix and passes after it.

## P1

### T-S01 — non-canonical FS base reaches `wrmsr` in ring 0
**Where:** `arch_prctl(ARCH_SET_FS)` and `clone(CLONE_SETTLS)`.
**Impact:** the user-supplied base was stored and written to `IA32_FS_BASE`
without validation, in ring 0 — in the syscall and again on every context
switch. On real x86-64 hardware and KVM, `wrmsr` of a non-canonical value raises
`#GP(0)` in ring 0, which the kernel treats as fatal: a ring-3 program could
panic the kernel. QEMU-TCG does not enforce this `#GP`, so CI observes the
missing validation rather than the panic.
**Fix:** a base must be a canonical user-half address (`< 0x0000800000000000`):
`arch_prctl` returns `-EPERM`, `clone` `-EINVAL`, before the PCB or the MSR is
touched.
**Regression:** `tls.set_fs_rejects_noncanonical` (`/bin/fsguard`).
**Status:** `PASS_QEMU`; the panic itself is `PENDING_HARDWARE`.

## P2

### T-V01 — the open access mode was not enforced
**Where:** `vfs_fd_read` / `vfs_fd_write`.
**Impact:** a file opened `O_RDONLY` could be written and an `O_WRONLY` file
read. A program could overwrite a file it was only allowed to open for reading.
**Fix:** a write to an `O_RDONLY` description and a read from an `O_WRONLY` one
return `-EBADF`.
**Regression:** `vfs.accmode_enforced_on_rw`; UFUZZ's generated programs expect
`-EBADF` for every write to their read-only fds (a removed guard made 98
generated programs fail).

### Failed `open()` had filesystem side effects
**Where:** `vfs_open`.
**Impact:** with the fd table full, `open(O_CREAT|O_TRUNC)` truncated or created
the target and then failed with `-EMFILE` — destroying a file's contents through
a call that reported failure.
**Fix:** the fd and the description are reserved before any create/truncate.
**Regression:** `vfs.failed_open_has_no_side_effect`.

### Two threads could be given the same fd slot
**Where:** every free-slot allocation (`open`, `pipe`, sockets, `F_DUPFD`).
**Impact:** the kernel is preemptible in ring 0; picking a free slot and
publishing the description were separate steps, so a sibling thread sharing the
table (`CLONE_FILES`) could receive the same slot and one description silently
replaced the other (e.g. a lost pipe end whose reader never sees EOF).
**Fix:** `fd_install` picks and publishes in one step with preemption deferred;
no unpublished slot is held across blocking I/O. Residual: if a sibling takes
the last free slot during a create/truncate, the open fails after its side
effect (strictly narrower than before; single-threaded callers are fully
covered).

### `fcntl(F_DUPFD)` floor handling and fds 0–2
**Where:** `vfs_fcntl`, `vfs_dup2`, `vfs_fstat`.
**Impact:** a negative floor was clamped to 0 and the floor was cast to `int`
before validation, so `F_DUPFD` with `-1` or `0x80000000` could install a
descriptor into an empty console slot 0–2 — silently replacing the program's own
standard input or output. `dup2` refused to copy an implicit console fd, and
`fstat` of a redirected fd 0–2 described the console instead of the installed
file. Impact is limited to the calling process.
**Fix:** fds 0–2 are always open (an empty slot is the console); `F_DUPFD`
validates the floor as a full-width value (`< 0` → `-EINVAL`, `≥ 32` →
`-EMFILE`) and never returns 0–2; `dup2` and `F_DUPFD` can copy the console; a
filled slot is described by its own file.
**Regression:** `vfs.fcntl_dupfd_floor_contract`,
`vfs.console_fds_are_always_open`, and every UFUZZ mode-5 program.

## Confirmed, fix pending a contract decision

### UF-F001 — zero-length `read`/`write` does not validate the fd
**Where:** `do_read` / `do_write` (`kernel/syscall/syscall.c`).
**Found by:** UFUZZ U8 structure-aware mutation (a length mutated to 0), in
three independent campaigns.
**Behaviour:** `if (count == 0) return 0;` runs before the fd is checked, so
`write(-1, buf, 0)`, `read(-1, buf, 0)`, a zero-length write to a never-opened
fd and a zero-length write to an `O_RDONLY` fd all return 0; the documented
contract (and Linux) return `-EBADF`. A non-zero length is validated correctly.
**Impact:** none on data or privilege — nothing is read or written. POSIX allows
a zero-length transfer on a regular file not to detect errors, so this is a
contract divergence, not a vulnerability.
**Reproducer:** `fuzz/u8/findings/UF-F001_zero_length_ebadf.ir`.

## Not fixed in 1.4.0 (open audit items, not confirmed vulnerabilities)

Static-analysis candidates without a fail-first reproducer yet: FPU/XMM state
across threads (no `fxsave`/`xsave`), the direction flag on interrupt entry,
`IA32_FMASK` and `TF`, the FS base across `execve`, and `unlink`'s errno for a
missing parent. A one-time 2-page growth on the first `pipe(NULL)` of a boot
(not a per-program leak: a repeated batch is net-zero) is unidentified. None is
claimed as a vulnerability until reproduced and classified.
