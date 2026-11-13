<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH 1.4.0 — UFUZZ: the syscall fuzzer that checks its own oracle 🦊🔍

**MakhOS** is a from-scratch x86-64 operating system kernel in freestanding C
and assembly, built brick by brick — a phase lands only when its tests are
green. 1.3.0 finished KFUZZ, the fuzzer that runs *inside* the kernel. 1.4.0
adds its counterpart: **UFUZZ**, which attacks the kernel the way a hostile
program does — through the syscall interface, from ring 3. **240 in-kernel
tests passing**, a 12×4 stress run clean.

## The headline

A fuzzer is only as good as its oracle, and an oracle that shares the
generator's mistakes is blind exactly where it matters. UFUZZ is built so that
every verdict can be checked:

- **Every generated call carries the result the ABI contract requires** — not
  "didn't crash", but `-EBADF`, slot 3, `O_RDWR`. Any returned errno must be one
  the inventory documents. Every replay pass must leave the fd table exactly as
  the worker found it.
- **A second, independent model re-derives every expectation** from the ABI
  inventory and the kernel's own headers, and the two must agree on all 4099
  generated programs. Mutation tests prove the agreement depends on both sides.
- **Every event becomes an artifact** — classified, deduplicated by signature,
  with the build it came from and a command that replays it in a fresh boot.
- **Findings shrink to their essence**: the reproductions of three planted
  kernel bugs minimize from 24–26 calls to 2, 1 and 7.
- **Coverage-guided mutation was measured, not assumed** — under a rule fixed
  before the campaigns ran. Result: no measurable difference on this surface
  (§U8). Reported as measured.

## UFUZZ, brick by brick

| Brick | What it adds | Doc |
|---|---|---|
| U0 | ABI inventory: 80 syscalls (48 implemented, 25 partial, 7 stub), every pointer's direction and size, every resource's lifecycle, the documented errnos | [UFUZZ_ABI](UFUZZ_ABI.md) |
| U1–U2 | Isolated ring-3 workers under a watchdog; net-zero page/heap accounting | [U3 design](UFUZZ_U3_DESIGN.md) |
| U3 | Typed program IR, dependency-aware generator, a Python structural validator | [U3 design](UFUZZ_U3_DESIGN.md) |
| U4 | Kernel `trace-pc` coverage with edge identity; ext2 block-map and fd-exhaustion modes | [U4 design](UFUZZ_U4_DESIGN.md) |
| U5 | Expected-result and documented-errno oracles; the fd surface (`pipe`, `dup2`, `fcntl`, `openat`, `socket`) with a baseline-relative fd model; independent re-derivation of every expectation | [U5](UFUZZ_U5_ORACLES.md) |
| U6 | Crash classification, triage, deduplicated artifacts, replay in a fresh boot | [U6](UFUZZ_U6_TRIAGE.md) |
| U7 | Programs run from their IR; ddmin minimization accepted only on the same signature at a declared threshold | [U7](UFUZZ_U7_MINIMIZE.md) |
| U8 | Corpus with provenance; structure-aware mutation; a host-driven generational campaign; the pre-registered measurement | [U8](UFUZZ_U8_CORPUS.md) |

## Kernel fixes

Every fix below has a test that failed before it and passes after it. Details
and severity in **[SECURITY_ADVISORY_1.4.0](SECURITY_ADVISORY_1.4.0.md)**.

- **T-S01** — a non-canonical FS base reached `wrmsr` in ring 0 (a ring-3
  kernel panic on real hardware; QEMU does not raise the `#GP`).
- **T-V01** — the open access mode was not enforced: a read-only fd could be
  written.
- A failed `open()` could truncate or create its target.
- Two threads sharing an fd table could receive the same slot.
- `F_DUPFD` floor handling, and fds 0–2 as always-open descriptors (`dup2` of a
  console fd, `fstat` of a redirected one).
- **UF-F001** — a zero-length `read`/`write` now validates its fd.

## The first natural finding — found, reproduced, fixed

**UF-F001** — found by U8's mutation engine in three independent campaigns, not
planted: a zero-length `read`/`write` returned 0 without validating the fd
(`write(-1, buf, 0)` == 0; the contract and Linux say `-EBADF`). No data or
privilege impact. Fixed: a zero-length call now reports exactly the fd error a
1-byte call would. The committed reproducer is a permanent regression test
(`ufuzz.uf_f001_zero_length_validates_fd`: 4 programs failed before the fix, all
pass after).

The same campaigns also corrected the independent model itself: 22 of the 26
programs the mutated arms flagged were U5.3 deriver errors (an fd bind takes
whatever the call returns), now fixed and pinned by tests.

## Definition of done (spec §13)

| # | Condition | Status |
|---|---|---|
| 1 | ABI inventory distinguishes implemented/partial/stub/unsupported | ✅ `PASS_QEMU` |
| 2 | Ring-3 workers run testcases, failures isolated | ✅ `PASS_QEMU` |
| 3 | External supervisor handles VM timeout/hang | ✅ runner timeout → `KERNEL_HANG`; crash → reboot (U7/U8) |
| 4 | Resource accounting detects unexplained drift | ✅ pmm/kheap net-zero + fd baseline |
| 5 | Typed programs generated, saved, replayed | ✅ |
| 6 | Kernel/user coverage separated by domain | ⚠️ kernel domain only; user/target coverage `NOT_IMPLEMENTED` |
| 7 | Oracles have fail-first tests and documented limits | ✅ |
| 8 | Crash artifacts complete and replayable | ✅ `--check` gate |
| 9 | Replay and dedup on a fixed regression set | ✅ U6 fixtures |
| 10 | Corpus import/export preserves programs | ✅ byte-identical |
| 11 | Race mode states single-CPU limits | ⚠️ U9 not built — no race mode exists, so none is claimed |
| 12 | A natural finding, only if verified | ✅ UF-F001 — found, reproduced, fixed |
| 13 | Published campaign results with commit/configuration/budget | ✅ [U8 §6–7](UFUZZ_U8_CORPUS.md) |
| 14 | Hardware-only checks marked | ✅ T-S01's `#GP` panic: `PENDING_HARDWARE` |
| 15 | README, release notes, advisory reflect what shipped | ✅ |

**Items 6 and 11 are open, and 1.4.0 is tagged with them open, by decision:**
user-space coverage domains and the single-CPU race mode (U9-A) are not built,
so nothing about them is claimed. Race testing moves to the SMP 2.0.0 line,
where it can be measured on more than one CPU instead of simulated on one.

## Claims register (spec §15)

| Claim | Status | Evidence |
|---|---|---|
| CL-UF-001 Worker crash does not stop the campaign | `PASS_QEMU` | `ufuzz.worker_lifecycle_contained` |
| CL-UF-002 Kernel/user coverage separated | `NOT_IMPLEMENTED` | kernel domain only |
| CL-UF-003 Reset leaks no resources | `PASS_QEMU` | net-zero batches + fd baseline guard; one unexplained one-time 2-page growth per boot is documented |
| CL-UF-004 Generator respects dependencies | `PASS_QEMU` | U3 validator over every generated program |
| CL-UF-005 MAKH-SAN catches the declared classes | `NOT_IMPLEMENTED` | — |
| CL-UF-006 Replay reproduces a finding | `PASS_QEMU` | U6/U7: 5/5, 3/3, 3/3 boots |
| CL-UF-007 Corpus sharing with KFUZZ is sound | `NOT_IMPLEMENTED` | shared envelope only; no adapter |
| CL-UF-008 Race mode replays a schedule | `NOT_IMPLEMENTED` | U9 |
| CL-UF-009 Target adapter separate from MAKH ABI | `NOT_IMPLEMENTED` | — |

## Run it

```sh
make fuzz-check                         # the 8 host-side UFUZZ gates (no QEMU)
make test                               # the in-kernel suite
make test-suite SUITE=ufuzz             # the UFUZZ in-kernel tests
make test-suite SUITE=ufuzzreplay REPLAY=5:3:23758       # replay one program
python3 tools/ufuzz_triage.py build/suite.log --check    # triage a run
python3 fuzz/u8/campaign.py guided 1 --gens 20 --batch 24 --out build/u8/g1
```

All results are `PASS_QEMU` (one CPU, QEMU-TCG) unless marked otherwise.

## Assets

| File | What it is |
|---|---|
| `makhos-1.4.0.iso` | Bootable ISO — `qemu-system-x86_64 -cdrom makhos-1.4.0.iso -vga std -m 128M` |
| `MAKH-1.4.0-src.tar.gz` | Full source tree (`make && make run`) |
| `MAKH-1.4.0-src.zip` | Full source tree (zip) |
| `SHA256SUMS` | Checksums for the three files above |

## Build from source

Needs `gcc`, `nasm`, `grub-mkrescue` (with `xorriso`) and `qemu-system-x86_64`.

```sh
tar xzf MAKH-1.4.0-src.tar.gz && cd MAKH-1.4.0
make            # build makhos.kernel + makhos.iso
make run        # boot it in QEMU
make test       # run the 240 in-kernel tests
make stress     # 24×4 stress campaign
make fuzz-check # the 8 UFUZZ host-side gates (no QEMU)
fuzz 2000       # drive KFUZZ over all targets from the shell
```

## License

Dual-licensed: **GNU AGPL-3.0** (see `LICENSE`) or a **commercial license**
(see `LICENSING.md`). Earlier revisions were BSD-3-Clause; see `NOTICE`.
