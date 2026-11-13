<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U7: reproduction and IR minimization

**Scope:** spec §4 U7. Reproduce a U6 finding, then shrink its program while the
finding stays the same — same U6 classification and signature, at a declared
reproduction threshold.
**Depends on:** U3 (IR, validator), U5.3 (deriver), U6 (records, triage,
signatures, `ufuzzreplay`).

All results below are `PASS_QEMU`.

## 1. Running a program from its IR

`/bin/ufuzz run-ir <file> <k> [--sum <path>]` parses program `k` of a batch
file and runs it through the same worker path as `cov`: fd baseline, replay,
out-buffer digest, errno contract, expectations. A batch file is

```text
# u7 batch count=<N>
### prog 0
<U3 IR with U5.3 =expect annotations and an optional `need` line>
### prog 1
...
```

Only the syscalls the generator emits are accepted. Any parse problem is a
harness failure (`UFZ1 diag ... cls=106 what=ir-parse`, controller outcome
OTHER), never a fault.

Delivery needs no ISO rebuild: `tools/u7_batch.py image BATCH.ir OUT.img`
copies `build/ext2.img` and writes the batch to `/u7/batch.ir` with `debugfs`;
the guest mounts it at `/mnt`. `make test-suite SUITE=ufuzzmin BATCH=<runs>
EXT2=<img>` boots with `makh.ufuzz.batch=<runs>` and runs every program of
`/mnt/u7/batch.ir` that many times, reporting through the U6 records (`src=ir`,
`a` = program, `b` = run). `BATCH`, `EXT2` and `TIMEOUT` are validated before
they reach the boot line or the runner.

**Equivalence (`ufuzz.u7_run_ir_matches_cov`):** six programs (modes 0, 1, 2,
5, 5, 6) are baked into the initrd as IR. Each runs as `cov` and as `run-ir`;
both must be clean and write identical digests of the return trace and the
out-buffers. (`getpid`'s result is excluded from the cross-process digest: it is
each worker's own pid.) **Rejection (`ufuzz.u7_run_ir_rejects_malformed`):**
unknown call, undeclared handle, bad number, missing program → outcome OTHER.

## 2. Minimization

`python3 fuzz/u7/minimize.py ARTIFACT_DIR [--tree ROOT]` on a U6 artifact of a
`cov` testcase, against the kernel built in `ROOT`:

| Spec step | Here |
|---|---|
| 1. replay the seed | `SUITE=ufuzzreplay REPLAY=m:b:c`, signature counted per run |
| 2. replay the serialized program | `original.ir` (generator's expectations) through `run-ir` |
| 3. delete syscalls | ddmin over calls; deleting a call also deletes every call using a handle it creates |
| 4. fewer processes/threads | NOT_APPLICABLE: generated programs are single-process |
| 5. shrink buffers/lengths | read/write length → 1; buffers cut to their largest use |
| 6. simplify flags/values | lseek offset → 0, `F_SETFL` argument → 0 |
| 7. retest every candidate | a whole round of candidates runs in one boot |

Every candidate must pass the U3 validator and is re-annotated by the U5.3
deriver: the generator's expectations do not survive an edit (slot numbers
shift, a deleted write changes what a read returns). A call the deriver cannot
predict drops the candidate, which also keeps blocking reads and `SIGPIPE`
writes out of the batch. Candidate order is fixed: the same results give the
same minimal program.

**Acceptance:** same classification and same U6 signature, reproduced at the
threshold — 3/3 runs for deterministic classes (oracle, leak, kernel
exception), ≥ 2/5 for `TIMEOUT` / `KERNEL_HANG` with the rate recorded and no
claim of determinism. A candidate that fails with a different signature is
rejected. A kernel crash ends the boot: the crashing candidate is recorded and
the rest run in the next boot; intermediate steps accept one crash, the final
program must crash in 3 of 3 boots.

**Output** (in the artifact directory): `original.ir`, `minimized.ir` (a
one-program batch), `minimize.json` — seed replay rate, original and minimized
reproduction, calls before/after, rounds, candidates, boots, steps, and the
minimized program's replay command. The U6 artifact is kept unchanged.

## 3. Validation

`make u7-check` (`fuzz/u7/selftest.py`, in `make fuzz-check` and the CI
`abi-check` job, which also checks that `debugfs` is installed) drives the
minimizer with a simulated kernel bug:

- re-annotation is lossless on unedited mode-5 programs (the deriver reproduces
  the generator's expectations exactly);
- 300 random deletions: every candidate valid and fully annotated, no dangling
  handle;
- the simulated bug shrinks to the two calls that trigger it, deterministically;
- if every reduction changes the signature, the original is kept;
- a finding that does not reproduce yields no minimized program;
- thresholds: 2/5 accepted for timing classes, 1/5 not; 2/3 not accepted for a
  deterministic class;
- a crash ends a boot; the remaining programs complete in the next one.

End-to-end results on real kernels: §4.

## 4. Measured minimizations

Each run: a kernel mutation applied to a copy of this tree, `make test-suite
SUITE=ufuzz`, U6 triage, then `minimize.py` on the most frequent finding of the
target class. Numbers are from `minimize.json`.

| Mutation | Finding | Seed replay | Original | Calls | Boots | Minimized |
|---|---|---|---|---|---|---|
| `F_GETFL` returns 0 | `ORACLE_VIOLATION` (fcntl, cmd 3) | 5/5 | 3/3 | 24 → **2** | 11 | 3/3 |
| `do_pipe` keeps both fds on `-EFAULT` | `RESOURCE_LEAK` (fd) | 5/5 | 3/3 | 24 → **1** | 5 | 3/3 |
| `vfs_dup2` takes no reference | `KERNEL_EXCEPTION` #PF in `file_put` | 1/1 | 1/1 | 26 → **7** | 20 | 3/3 boots |

The minimized programs (expectations from the deriver):

```text
# F_GETFL returns 0
call socket imm:2 imm:2 imm:0 -> fd:6 =slot:0
call fcntl res:6 imm:3 imm:0 =exact:2

# do_pipe keeps both fds on -EFAULT (non-canonical pointer; the fd guard sees the leak)
call pipe bad:140737488355328 =exact:-14

# vfs_dup2 takes no reference: two closes free a description fd A still holds,
# the pipe reuses the memory, close(A) faults in file_put
call open path:0 imm:0 -> fd:2 =slot:0
call fcntl res:2 imm:0 slot:2 -> fd:3 =slot:2
call dup2 res:3 slot:4294967307 -> fd:4 =slot:11
call close res:4 =exact:0
call close res:3 =exact:0
call pipe buf:0 -> fd:10,11 =exact:0
call close res:2 =exact:0
```

**Not reproduced, reported as such:**

- The `F_GETFL` finding run against the unmutated tree: seed replay 0/5,
  original 0/3 → `not-reproduced`, no `minimized.ir`.
- A `vfs_dup2` finding first seen after a 48-program warm-up batch (seed 5:8)
  ran clean in a fresh boot (seed replay 0/5): that crash depends on state left
  by earlier programs (the use-after-free needs a reused allocation), so it is
  not reproducible in isolation. The table above minimizes a self-contained
  input of the same signature (5:9, crashes on the first run of a fresh boot).

Kernel-crash signatures name functions when the run's kernel ELF is available
(U6 checks its build id); the minimizer symbolizes with the tested tree's kernel
so its signatures match the artifact's.

## 5. Limits

- One program at a time: a finding that needs state from earlier programs is
  reported `not-reproduced`; sequence minimization is not implemented.
- Value simplification covers lengths, lseek offsets, `F_SETFL` arguments and
  buffer sizes only.
- Full minimizations need QEMU and run locally; CI runs the selftest and the
  in-kernel `run-ir` equivalence and rejection tests.
