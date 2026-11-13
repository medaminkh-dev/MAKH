<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U6: crash classification, triage and artifacts

**Scope:** spec §4 U6. Turn every event of a test run into a classified,
deduplicated, replayable artifact — from the serial log alone, on the host.
**Depends on:** U1 (worker isolation, watchdog), U2 (net-zero accounting),
U5 (oracles and their diagnostics).
**Not covered:** minimization (U7), root-cause grouping across different
symptoms (spec §10.3).

All results below are `PASS_QEMU`.

## 1. Pipeline

```text
 guest (QEMU)                                         host
 ───────────────────────────────────────────────      ─────────────────────────────
 kernel     UFZ1 build / UFZ1 ufault                  tools/run_tests.py  → serial log
            exception and panic reports               tools/ufuzz_triage.py LOG...
 KTEST      UFZ1 tcb / tc / drift / edges / replay      parse → join → classify
 /bin/ufuzz UFZ1 res (controller)                       → dedup by signature
            UFZ1 diag / trace (worker)                  → build/ufuzz-artifacts/<sig>/
```

The guest test disk runs `snapshot=on`, so nothing survives a boot except the
serial log; every record is one line of integers and fixed tokens.

| Record | Printed by | When | Fields |
|---|---|---|---|
| `UFZ1 build` | kernel (`makh.test`) | boot | `id` = `git describe --always --dirty` at build |
| `UFZ1 tcb` | KTEST | before each spawn | `n` (run-wide), `src` cov/selfcheck, `a b c` = mode/base/mut or kind |
| `UFZ1 tc` | KTEST | after each run | `n`, `cpid`, `status`, `expect` ok/fail |
| `UFZ1 res` | controller | non-clean run | `src cpid wpid a b c outcome status` |
| `UFZ1 diag` | worker | self-check failure | `pid pass cls what diag call` [+ `nr emode ret want a0 a1`] |
| `UFZ1 trace` | worker | self-check failure | per-call returns of the failing pass |
| `UFZ1 ufault` | kernel | ring-3 CPU exception | `pid vec err rip cr2` |
| `UFZ1 drift` | KTEST | batch not net-zero | `dpmm dheap` |
| `UFZ1 edges` | KTEST campaign | each run | new coverage edges |

`tcb` is printed before the spawn so a kernel crash or hang inside a testcase is
still attributable to its inputs. The enclosing `[ RUN ]` line names the
campaign. Joins: `tc.cpid → res.cpid`, `res.wpid → diag/trace/ufault.pid`.

## 2. Classification

| Class | Source | Not a finding when |
|---|---|---|
| `ORACLE_VIOLATION` | worker cls 101 expectation, 102 data-check, 103 replay, 104 digest, 107 contract | — |
| `RESOURCE_LEAK` | cls 108 with diag 0x7NN / 0x78N (fd state after pass 1 / 2); `UFZ1 drift` | — |
| `INVARIANT_FAILURE` | cls 108 diag 0x74N (F_GETFL / fstat disagree); a KTEST assertion no other finding explains | — |
| `USER_FAULT` | controller outcome USER_FAULT + `UFZ1 ufault` | — |
| `TIMEOUT` | watchdog SIGKILL | — (not by itself a deadlock) |
| `KERNEL_EXCEPTION` | `!!! EXCEPTION CAUGHT !!!` | — |
| `KERNEL_PANIC` | `*** KERNEL PANIC ***` | — |
| `KERNEL_HANG` | runner timeout, no exception or panic | — |
| `HARNESS_FAILURE` | cls 105 capacity, 106; 0x7FD/0x7FE precondition; a run without a worker record; **a negative canary that passes** | — |
| `INFRA_FAILURE` | no QEMU exit status (build/launch failed); spawn failure | — |
| `EXPECTED_ERRNO` | a call that returned the -errno its contract requires | always: counted, never a finding |
| `TARGET_CRASH` | — | `NOT_IMPLEMENTED` until U10 |

A testcase with `expect=fail` (selfcheck canaries 0–3, 5, 7, 9, 11; EMFILE
control mode 4) that fails is "failed as designed" and is counted, not
reported. One that passes is a `HARNESS_FAILURE`: the oracle it guards is blind.

Confidence: `high` when the record chain is complete (testcase → controller →
worker or open testcase → crash); `medium` when a crash or drift has no
testcase; `low` for a non-clean run with no worker record.

## 3. Signatures and deduplication

Signatures are computed from the log alone (never from a regenerated program),
so they do not depend on what is installed on the triage host:

| Class | Signature |
|---|---|
| `ORACLE_VIOLATION` | oracle, syscall, kind of returned result, kind of required result, selector (fcntl/ioctl `cmd`, arch_prctl `code`) |
| `RESOURCE_LEAK` (fd) | pass |
| `USER_FAULT` | vector, error code, user RIP |
| `KERNEL_EXCEPTION` | vector, error code, CR2 class (null page / user half / kernel half), top 3 frames |
| `KERNEL_PANIC` | message with numbers removed, top 3 frames |
| `KERNEL_HANG` / `TIMEOUT` | campaign / source, mode |

Frames are function names when `--kernel` is the ELF that produced the log
(its build id is checked), raw addresses otherwise. The call index is not part
of an oracle signature: the same fault appears at different positions in
different programs. How the expectation was formed (exact flags vs baseline
flags) is not part of it either. Duplicates increase `occurrences` and list
every other input.

Deduplication is per symptom. One root cause can produce several signatures
(the dropped `-EFAULT` cleanup below yields six); grouping them is root-cause
correlation, spec §10.3, and is not claimed here.

## 4. Artifacts

`build/ufuzz-artifacts/<16-hex signature>/`:

- `finding.json` — classification, subkind, reason, confidence, signature,
  occurrences, inputs (campaign, testcase, mode/base/mut or kind, new edges),
  build id, ABI schema, PRNG, QEMU command line, worker budget, worker pid,
  diag fields, trace, fault vector/error/RIP/CR2, backtrace, resource deltas,
  replay command, other inputs of duplicates;
- `log.txt` — the log excerpt around the event;
- `program.ir` — the generated program, regenerated with `build/u4gen`
  (UFUZZ testcases only).

Directory names are hashes; replay commands are built from parsed integers. No
log text reaches a path or a shell command.

`--check` fails if any artifact lacks a required field (a placeholder such as
`unknown` counts as missing). `--fail-on-findings` fails if the run has any
finding.

## 5. Replay

`make test-suite SUITE=ufuzzreplay REPLAY=<mode>:<base>:<mut>` boots with
`makh.ufuzz.replay=…` and runs that one program 5 times through the normal
controller and worker path, printing each status and the number of non-clean
runs. Without the cmdline key the suite does nothing. `REPLAY` is validated
(`digits:digits:digits`) before it reaches the boot line. KFUZZ panics carry
their own replay (`fuzz replay <seed> <target>` in the kernel shell).

Measured: the oracle-violation finding below replays non-clean 5/5 in a fresh
boot; the kernel-exception finding replays the same page fault (CR2 `0x4`) on
the first run.

## 6. Validation

`make u6-triage-check` (`fuzz/u6/selftest.py`, part of `make fuzz-check` and
the CI `abi-check` job). Real fixtures are serial logs of actual runs filtered
to the lines triage reads (`filter_log` in the selftest):

| Fixture | Run | Result |
|---|---|---|
| `clean.log` | ufuzz suite, clean tree | 0 findings; 488 testcases, 9 failed as designed |
| `oracle_getfl.log` | kernel mutation: `F_GETFL` returns 0 | `ORACLE_VIOLATION` ×17 → 1 signature |
| `kexc_dup2.log` | kernel mutation: dup2 takes no reference | `KERNEL_EXCEPTION` #PF in `file_put`, attributed to its testcase |
| `kexc_dup2_replay.log` | that finding's replay command | same signature: 1 signature ×2 across two boots |
| `leak_pipe_efault.log` | kernel mutation: `do_pipe` keeps fds on `-EFAULT` | `RESOURCE_LEAK` fd + oracle symptoms: 6 signatures, none shared with the `F_GETFL` fixture |
| `kfuzz_watchdog.log` | stress run, KFUZZ watchdog panic | `KERNEL_PANIC`, replay `fuzz replay <seed> ext2img`; `--check` flags the missing build id |

Derived cases (one stated edit of a real fixture each): kernel hang, a passing
canary, same RIP with a different vector (must not merge), worker fault, worker
timeout, batch drift, failed build, and hostile record values (artifact paths
stay hashes). Two mutations of the triage tool itself (signature without the
required-result kind; passing canaries ignored) each fail the selftest.

CI `build-and-test` runs `tools/ufuzz_triage.py build/suite.log --check
--fail-on-findings` on its own suite log and uploads the artifacts on failure.
