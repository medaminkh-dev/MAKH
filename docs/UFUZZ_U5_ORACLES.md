<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U5 oracles: U5.0 – U5.3

**Scope:** the result oracles added on top of U0–U4 (spec §4, U5 — Oracle
Framework). U5.0 and U5.1 judge *what a call returned*; U5.2 extends the
generated surface to the descriptor syscalls and adds an fd-state oracle.
**Depends on:** U0 (ABI inventory), U1 (worker isolation), U2 (net-zero
accounting), U3 (typed IR + validator), U4 (shared generator, coverage modes).
U5.3 (§9) re-derives every generated expectation with an independent Python
model, so the generator and the oracle cannot share one mistake unnoticed.

All results below are `PASS_QEMU`. Nothing here is a hardware claim.

## 1. Oracle summary

| Oracle | Where | Fails with | Positive canary | Negative canary |
|---|---|---|---|---|
| Expected result (U5.0) | `u3_run` | `expectation` (0x1NN, NN = call) | every clean generated program | selfcheck 0, 1, 7 |
| Documented errno (U5.1) | `u3_run` | `contract` (0x6NN) | selfcheck 6, 13 | selfcheck 5 |
| fd baseline / isolation (U5.2) | `u3_worker_pg` | `isolation` (0x7NN, 0x7(80\|NN)) | selfcheck 10 | selfcheck 9 |
| fd-slot precondition (U5.2) | `u3_worker_pg` | `isolation` 0x7FE | selfcheck 12 | selfcheck 11 |
| fd probe agreement (U5.2) | `u3_fdsnap` | `isolation` 0x74N | — | — |
| Independent re-derivation (U5.3, host) | `fuzz/u5/derive.py` | `U5 DERIVE-CHECK: FAIL` | unmutated generator/deriver pair (0 contradictions) | mode 4 control; 3 wrong annotations; 6 mutations |

A worker that fails a self-check exits with a class code in 1..127
(`U3X_EXPECT` 101 … `U3X_ISOLATION` 108), never with the raw diagnostic: the
kernel keeps only the low 8 bits of an exit status. On an expectation failure
the worker also prints the syscall number, the expectation kind, the returned
value and the required value (`nr= mode= ret= want=`), so a not-clean run is
attributable from the serial log alone.

The KTEST `ufuzz.selfcheck_failures_are_never_ok` requires every negative
canary to be reported as a failure; `ufuzz.contract_oracle_accepts_documented_errno`,
`ufuzz.replay_restores_fs_base` and `ufuzz.fd_oracles_accept_correct_programs`
require every positive canary to pass. An oracle without both is not counted.

## 2. U5.0 — generated calls carry their required result

Before U5.0 every generated call was `U3_ANY`: crashes, hangs, leaks and replay
divergence were visible, a wrong return value was not. The generator knows what
it built, so each `u3_call` now carries `expect_mode` / `expect_val`:

| Mode | Meaning |
|---|---|
| `U3_ANY` | no requirement (baked U3 programs only) |
| `U3_NONNEG` | `ret >= 0` |
| `U3_NEG` | `ret < 0` |
| `U3_EXACT` | `ret == expect_val` |
| `U3_SLOT` (U5.2) | `ret ==` baseline-free slot `expect_val` (§4.2) |
| `U3_BASEFL` (U5.2) | `ret ==` baseline `F_GETFL` of fd `expect_val` |

- **Skeleton mode:** open/mmap/getpid/read non-negative; close/mprotect/munmap
  exactly 0; `arch_prctl` SET_FS/GET_FS exactly 0 and an unknown code exactly
  `-EINVAL`; write exactly `-EBADF` (every skeleton fd is `O_RDONLY`).
- **ext2 mode:** from a file-size model — lseek returns the offset, write
  returns `n`, the read at offset 0 returns `min(32, size)`, close and unlink 0.
- **EMFILE mode:** 29 opens succeed, the 30th is exactly `-EMFILE`, each close
  exactly 0, closing the failed handle exactly `-EBADF`.

**Mutation result:** removing the T-V01 access-mode guard from `vfs_fd_write`
made 98 generated programs not clean.

## 3. U5.1 — a returned errno must be documented

Any return in `[-4095, -1]` must be an errno the ABI inventory lists for that
syscall (`fuzz/abi/makh_syscalls.json`, `"errno"`). Otherwise the worker fails
with `contract` (0x6NN). This applies to generated and baked programs, and
independently of the expectation: a call expected to fail must also fail with a
*documented* errno.

- `fuzz/abi/emit_errno.py` generates `user/ufuzz_abi_errno.h` from the
  inventory and `kernel/include/errno.h`. It fails closed on an errno name the
  kernel does not define. `--check` verifies the header is fresh; it runs in
  `make abi-check` and in CI.
- Canaries: selfcheck 5 (`close(-1)` judged against `getpid`'s empty errno set)
  must fail; selfcheck 6 (the same call against its own contract) must pass.

The errno oracle and the inventory constrain each other: when a correct kernel
result trips it, the inventory is incomplete and is corrected (as with fcntl's
`EMFILE`, §5); when an undocumented errno is wrong, the kernel is.

## 4. U5.2 — descriptor surface

### 4.1 Calls and modes

New generator calls: `pipe`, `pipe2`, `dup2`, `fcntl`, `openat`, `socket`
(alongside `open`, `read`, `write`, `close`). Two modes:

| `cov` mode | Builder | Purpose |
|---|---|---|
| 5 `fdops` | `u3g_build_fdops(base, mut)` | random descriptor programs; base fixes the operations, mut the argument values |
| 6 `pipe-rollback` | `u3g_build_piperb()` | every Ring-3-reachable `vfs_pipe` failure path |

`fuzz/u4/validate.py` sweeps both through the Python structural validator
(1024 mode-5 seeds per run of `make fuzz-check`).

### 4.2 Baseline-relative fd model

A worker does not assume which fds it inherits. Before pass 1 it records, for
every fd 0..31, the `F_GETFL` result (or "closed"), cross-checked against
`fstat`. The fds ≥ 3 that are free at that point, in ascending order, are the
**slot ordinals**:

- operand `slot:k` (`U3_FDSLOT`) resolves to the k-th baseline-free fd; any
  bits above the low 16 are ORed back, so `slot:k | 1<<32` exercises the
  32-bit truncation of an fd argument;
- expectation `U3_SLOT k` requires exactly that fd.

The generator keeps one model of the fd table, in ordinals, through every call
that allocates or releases a descriptor: open, openat, pipe, pipe2, dup2
(including its implicit close of the target), `F_DUPFD`, socket, close. Because
allocation is lowest-free and a program never touches an fd it did not create,
"lowest free fd ≥ slot(k)" is always slot(lowest free ordinal ≥ k), whatever
the worker inherited. fork, execve and exit also change fd state but are not in
the worker menu; the model documents them (the child gets a copy, execve keeps
every fd, exit releases all).

A program that depends on how many slots are free declares it:
`need_free` = N, either a minimum or, with `need_exact`, an exact count.
Mode 5 models 12 ordinals and never holds more than 8 fds, so it requires
at least 12 free slots and predicts `-EMFILE` only from the absolute `F_DUPFD`
bound. Modes 3, 4 and 6 predict exhaustion and require exactly 29.

### 4.3 Replay isolation

Replay runs a program twice in one process. The rule (documented at the `U3G_*`
enum in `user/ufuzz_u3_gen.h`): anything a call changes must be back to its
starting value before pass 2. For descriptors this is now checked directly:
the fd snapshot after pass 1 and after pass 2 must equal the baseline. Programs
therefore:

- close every fd they create (balance closes at the end);
- never `dup2` onto an fd they did not create, and never target fds 0–2;
- never `F_SETFL` a console copy (the description is shared with an inherited
  fd);
- read from a pipe only what is buffered, or after every writer is gone (a read
  never blocks); write only while a reader exists and the pipe has room (no
  `SIGPIPE`, no blocking).

The return trace and the out-buffer digest remain the backstop for state that
changes a result. Canary 9 is single-pass so that only the fd snapshot can see
its leaked fd.

### 4.4 Argument classes

| Class | Cases | Expected |
|---|---|---|
| invalid pointer (`bad:`) | `pipe`/`pipe2` with NULL, `0xffffffff80000000`, `0x0000800000000000`, `0x1000` | `-EFAULT`, no fd left behind |
| `-1` / out of range | `dup2(-1, x)`, `dup2(fd, -1 \| 32 \| 0x7fffffff)`, `fcntl(-1 \| 32 \| 0x7fffffff, F_GETFL)`, `close(-1)` | `-EBADF` |
| closed descriptor | `close`/`fcntl`/`dup2` on a free slot | `-EBADF` |
| invalid flags / commands | `fcntl(fd, 999)`; `F_SETFL` with access-mode bits | `-EINVAL`; access mode unchanged |
| large values | `F_DUPFD` floor 32, `0x7fffffff`, `0x80000000` | `-EMFILE` |
| negative floor | `F_DUPFD` floor -1 | `-EINVAL` |
| truncation | `dup2(fd, slot:k \| 1<<32)` | slot k |
| self / aliasing | `dup2(x, x)`; `dup2` onto another live fd; `F_SETFL` then `F_GETFL` through a duplicate | `x`; target fd; shared flags |
| unsupported family | `socket(99, SOCK_DGRAM, 0)` | `-EAFNOSUPPORT` |
| exhaustion (mode 6) | `pipe` with one free slot, with none | `-EMFILE`, then an open gets exactly the freed slot |

### 4.5 Descriptor contract (fds 0–2 and fcntl)

- fds 0–2 are always open. An empty slot is the implicit console; a filled slot
  is whatever was installed there.
- `dup2` and `F_DUPFD` accept an empty slot 0–2 as the source; the copy is a
  new open-file description on a static console vnode (`O_RDWR`, character
  device, accepted by the tty ioctls).
- `dup2(x, x)` returns `x` unchanged. Only `dup2` can target 0–2.
- `F_DUPFD`: lowest free fd ≥ arg, never 0–2 (arg 0..2 means ≥ 3);
  arg < 0 → `-EINVAL`; arg ≥ 32 or no free slot → `-EMFILE`. The floor is judged
  as a full-width value before any narrowing.
- `F_GETFD` is always 0 and `F_SETFD` is accepted: `FD_CLOEXEC` is not tracked
  (fcntl is `partial` in the inventory). `F_SETFL` changes only `O_APPEND` and
  `O_NONBLOCK`.
- `fstat` of a filled slot 0–2 describes the installed file.

## 5. Kernel and inventory changes made under U5.2

Each change has a test that failed before it and passes after it.

| Change | Test |
|---|---|
| fcntl documents `EMFILE`; dup2/fcntl document `ENOMEM` | selfcheck 13 |
| `F_DUPFD` floor < 0 → `-EINVAL`, ≥ 32 → `-EMFILE` | `vfs.fcntl_dupfd_floor_contract` |
| `F_DUPFD` floor 0..2 starts at 3; a floor above the lowest free fd is honoured | `vfs.fcntl_dupfd_floor_contract` |
| empty slot 0–2 is a valid dup source; `dup2(1, 1)` returns 1 | `vfs.console_fds_are_always_open` |
| `fstat` of a filled slot 0–2 | `vfs.console_fds_are_always_open` |

`kheap_test_fail_after(n)` (test-only, disarmed by default, no Ring-3
interface) makes the (n+1)-th kmalloc/kcalloc return NULL once.
`vfs.pipe_alloc_failure_rolls_back` uses it to fail each of `vfs_pipe`'s four
allocations in turn with interrupts disabled, and requires `-ENOMEM` with the
fd table and heap usage unchanged.

## 6. Mutation testing

Each mutation was applied alone to the kernel, the `ufuzz` and `vfs` suites
were run, and the mutation was reverted.

| Mutation | Detected by |
|---|---|
| `vfs_pipe`: no rollback of the read end when the write-end install fails | mode 6 |
| `vfs_pipe`: one allocation not freed on `-ENOMEM` | `vfs.pipe_alloc_failure_rolls_back` (not reachable from Ring 3) |
| `do_pipe`: no close of both ends on `-EFAULT` | mode 6; 24/48 mode-5 programs; selfcheck 8 |
| `vfs_dup2`: no reference taken on the shared description | kernel page fault during mode-5 programs |
| `F_GETFL` returns 0 | 17/48 mode-5 programs; `vfs.console_fds_are_always_open` |
| `F_DUPFD` ignores its floor | 15/48 mode-5 programs; `vfs.fcntl_dupfd_floor_contract` |

## 7. Tests and validation

| Test | Checks |
|---|---|
| `ufuzz.u52_fdops_programs_run_clean` | 48 mode-5 programs clean; pmm and kheap net-zero over the batch |
| `ufuzz.u52_pipe_failure_paths_roll_back` | mode 6 clean; pmm and kheap net-zero |
| `ufuzz.fd_oracles_accept_correct_programs` | selfcheck 8, 10, 12, 13 pass |
| `ufuzz.selfcheck_failures_are_never_ok` | selfcheck 7, 9, 11 (with 0–3, 5) fail |
| `vfs.fcntl_dupfd_floor_contract`, `vfs.console_fds_are_always_open`, `vfs.pipe_alloc_failure_rolls_back` | §5 |
| `make u5-derive-check` (host, in `make fuzz-check` and the CI `abi-check` job) | §9: every expectation re-derived; mutation selftest |

Commands: `make fuzz-check` (includes `make u5-derive-check`); `make test-suite SUITE=ufuzz`;
`make all makhos-test.iso && python3 tools/run_tests.py makhos-test.iso --timeout 900`;
`make makhos-stress.iso && bash tools/stress_tests.sh 12 4 makhos-stress.iso`.

## 8. Known limits

- U5.3 checks the generator's expectations against a second model on the
  host; it does not run the kernel. A kernel that disagrees with both models is
  caught by the guest executor, not by U5.3.
- `ioctl` on a *filled* slot 0–2 (e.g. stdout redirected to a pipe) still
  reports a tty.
- `lseek` on a console copy returns an offset (Linux: `-ESPIPE`).
- `FD_CLOEXEC` is accepted but not tracked; `pipe2` flags are ignored.
- The single-CPU model holds; no claim is made about SMP descriptor races.

## 9. U5.3 — independent re-derivation of expected results

The guest checks the kernel against the C generator's expectations. If the
generator's model of a syscall is wrong, the oracle is wrong the same way and a
kernel bug that matches the mistake passes. U5.3 closes that gap on the host.

### 9.1 Mechanism

- `build/u4gen` writes each call's expectation into the IR as an annotation
  (`=exact:-9`, `=slot:3`, `=nonneg`, `=basefl:1`, `=any`) and the program's
  precondition as `need N min|exact`. `fuzz/u3/u3.py` parses both and
  round-trips them; the structural validator ignores them.
- `fuzz/u5/derive.py` walks every program with its own model — fd table in
  baseline ordinals, open-file descriptions (flags, reference counts), pipes
  (buffered bytes, readers, writers), file existence and size, live mappings —
  and predicts each call's outcome. Its inputs are the calls and operands, the
  ABI inventory (argument C types decide integer narrowing; documented errnos)
  and the contract in §4.5. Numeric constants (`O_*`, `F_*`, `SEEK_*`,
  `ARCH_*`, `AF_*`/`SOCK_*`, errno values, `PIPE_CAP`, `VFS_MAX_FDS`) are read
  from the kernel headers and sources, not from the generator. The annotation
  is read only by the comparison.
- A program with no `need` line declares no free slots. The deriver then
  *assumes* an allocation succeeds but does not claim its slot (the prediction
  is downgraded to `nonneg`); every such assumption is counted in the report.

### 9.2 Verdicts

| Verdict | Meaning | Effect |
|---|---|---|
| AGREE | the C expectation is exactly the prediction | — |
| WEAKER | the C expectation accepts the prediction but also other outcomes (e.g. `nonneg` where the slot is known) | counted per syscall, reported |
| UNPREDICTABLE | the model cannot predict the call, or the C expectation is stronger than the model can confirm | counted; **fails** in modes 5 and 6 |
| CONTRADICTION | the C expectation rejects the prediction | **fails** |

Every predicted `-errno` must also be in the inventory's documented set for the
syscall. Mode 4 (the EMFILE negative control) must produce a contradiction.
Three hand-written programs with a wrong annotation (slot off by one, `-EBADF`
where 0 is due, `F_SETFL` keeping an access-mode bit) must be detected.

WEAKER expectations are reported, not tightened: U5.3 measures how strong the
existing oracle is; it does not change the expectations to agree with itself.

### 9.3 Mutation selftest

`derive.py --mutation-selftest` proves the comparison depends on both sides.
It compiles three mutated copies of the generator (the guest sources are not
modified) and perturbs three deriver rules, and requires each to produce
contradictions over 256 mode-5 programs while the unmutated pair produces none:

| Side | Mutation | Contradictions (256 programs) |
|---|---|---|
| generator | `F_SETFL` keeps access-mode bits | 6 |
| generator | pipe read expects the full request | 31 |
| generator | `dup2` onto a live fd expects the source slot | 279 |
| deriver | `F_SETFL` mask includes access-mode bits | 6 |
| deriver | pipe read returns the full request | 31 |
| deriver | `F_DUPFD` floor 0..2 may return 0..2 | 500 |

### 9.4 Result

`make u5-derive-check` (part of `make fuzz-check` and the CI `abi-check` job),
4099 programs:

| Mode | Programs | Agree | Weaker | Unpredictable | Contradiction |
|---|---|---|---|---|---|
| 0 skeleton | 1024 | 10157 | 0 | 0 | 0 |
| 1 ext2 | 1024 | 13308 | 0 | 0 | 0 |
| 2 ext2-direct | 1024 | 13308 | 0 | 0 | 0 |
| 3 emfile | 1 | 31 | 29 (`open`) | 0 | 0 |
| 4 emfile-control | 1 | 28 | 28 (`open`) | 0 | 2 (expected) |
| 5 fdops | 1024 | 25282 | 0 | 0 | 0 |
| 6 pipe-rollback | 1 | 61 | 0 | 0 | 0 |

Modes 0–2 rest on 1441 / 1024 / 1024 assumed allocations (`open`, no `need`).
The only weaker expectations are the EMFILE programs' `open` calls (`nonneg`
where the slot is determined).
