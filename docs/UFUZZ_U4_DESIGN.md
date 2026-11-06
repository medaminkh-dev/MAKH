<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U4 Design Proposal: coverage-guided generation

**Status:** v2 — architecture approved in review (Model A + C runtime generator);
this revision locks in the two points the review required *before* code:
edge-IDENTITY novelty (not a bit count) and per-seed attribution. Mirrors the
U3 design-review flow.
**Depends on:** U0 (ABI), U1 (ring-3 executor + watchdog), U2 (reset oracle),
U3 + U3.1 (typed programs: imm/res/null/buf/path, variant, iovec).

## 1. Goal, stated honestly

U4 adds a **feedback loop**: prefer programs that reach *new kernel code* over
undirected random ones, so a bounded campaign explores more of the syscall-
reachable kernel per unit time.

U4 is about **exploration efficiency, not bug-finding**. Bugs are still caught
by the oracles we already have — U1 (a worker fault/hang is contained and
classified), U2 (net-zero page/heap drift), U3 (per-call expectation, replay
trace, out-buffer digest). U4 only changes *which programs* those oracles see.
"Coverage went up" is never reported as "a bug was found". That distinction is
the whole point of naming this brick accurately.

## 2. What already exists (reuse, don't reinvent)

- `kernel/kfuzz/kfuzz.c`: a 65536-edge `cov_map` fed by the compiler via
  `__sanitizer_cov_trace_pc` (SanitizerCoverage `trace-pc`), with
  `kfuzz_cov_reset()`, `kfuzz_cov_count()` (popcount) and a `cov_enabled` gate.
- The KFUZZ campaign loop is already coverage-guided: pick a seed (fresh, or a
  mutated corpus entry), `before = cov_count()`, run, `after = cov_count()`,
  keep the seed iff `after > before`. A priority watchdog thread catches hangs.
- `COV_SOURCES` in the Makefile instruments vfs/path/tmpfs/tar, uvm/vmspace/
  pmm/page/kheap, tty, signal, net/*, elf, string, shell with
  `-fsanitize-coverage=trace-pc`.

U4 is "point the existing coverage machinery at ring-3-driven syscalls, and
grow a corpus from the result." The map, the popcount, the keep-if-new rule and
the watchdog are all reusable as-is.

## 3. The crux: coverage is ring-0, the controller is ring-3

Two models. The recommendation is **Model A for the U4 MVP**, with Model B
deferred to U4.1 only if we want the loop entirely in userspace.

### Model A (recommended) — ring-0 campaign driver, ring-3 execution

A new in-kernel KTEST (`ufuzz.coverage_guided_*`) is the campaign driver. It
reads `cov_map` **directly** (it runs in ring-0), and drives ring-3 execution by
spawning workers `/bin/ufuzz cov <seed0> <count>`; each worker generates
`count` typed programs from the seed range and runs them through the existing
`u3_run` executor, then exits. Between batches the driver reads the coverage
delta and keeps seeds that added edges.

- **Pros:** no new syscall and no new sandbox surface (a ring-3 process never
  touches kernel coverage); reuses the proven KFUZZ watchdog + cov API;
  deterministic and fully CI-bounded; the worker lifecycle is exactly U1's.
- **Cons:** the corpus/mutation logic lives in the ring-0 driver, not in
  `/bin/ufuzz`. Execution is still ring-3; only the *bookkeeping* is in-kernel.

### Model B (deferred, U4.1) — ring-3 controller via a gated coverage syscall

A fuzz-only `SYS_FUZZ_COV` (subops enable/reset/count) lets `/bin/ufuzz` run the
whole AFL loop in ring-3: in-guest corpus, read coverage between worker runs.

- **Pros:** the fully-userspace controller the UFUZZ vision describes.
- **Cons:** new ABI surface; a ring-3 path that reads/clears global kernel
  coverage is a sandbox concern that must be **airtight** (compiled only in
  test ISOs, refused in production, `is_user` + build-flag gated). The reward
  over Model A is architectural purity, not more bugs found. Not worth the risk
  for the MVP.

**Cross-process coverage note (both models):** `cov_map` is global kernel state,
so a forked worker's syscalls bump the same map the driver reads after `wait4`.
The fork/exec/wait machinery itself adds a roughly-constant per-run coverage
contribution, which the warmup + virgin-map below absorb (see §3.5).

## 3.5 Coverage measurement & per-seed attribution (the part the review flagged)

A bit COUNT is not enough: `cov_count()` tells us *how many* edges are set, not
*which* ones, and not *which seed* set a new one. If a batch runs many programs
and the count rises, we cannot say which program earned it. Getting this wrong
is the real failure mode (see the pre-mortem, §10): we would ship something that
calls itself coverage-guided while its feedback attribution is noise. So:

- **Two maps.** `cov_map` is the per-run (current) map; the driver keeps a
  second **`virgin`** map (ring-0, `COV_WORDS` words) of every edge seen so far
  in the campaign. Novelty of a run is `current & ~virgin` — the edges it hit
  that nothing had hit before — folded in with `virgin |= current`. A new kernel
  helper does the fold+count in one locked pass, e.g.
  `uint64_t kfuzz_cov_fold_new(uint64_t* virgin)` (reuses `cov_map`; no raw map
  leaves the kernel).
- **One program per worker (attribution over fork-thrift).** The driver resets
  `cov_map`, spawns `/bin/ufuzz cov <seed>` for exactly ONE program, waits, then
  computes that program's novelty. This binds novelty to a single seed — exact
  attribution. The batch-amortisation from v1 is DROPPED: it is precisely what
  destroys attribution. The fork+exec+wait cost is bounded by K (§7); if it ever
  dominates CI time, the escape hatch is U4.1/Model B, where the worker reads
  coverage in-process between programs and batching regains attribution — i.e.
  attribution precision is itself the argument for Model B later, not for
  batching now.
- **Explicit reset + warmup.** The campaign starts with `cov_reset()` and a
  zeroed `virgin`, runs ONE warmup worker whose map is folded into `virgin` and
  discarded — this absorbs the boot/spawn/exec/wait edges that every run shares,
  so from the first measured program, `current & ~virgin` reflects the PROGRAM,
  not the lifecycle around it.

## 4. Generation strategy

The worker needs to turn a seed into a typed program **at runtime** (we can't
bake an unbounded set). Recommendation: a **C port of the constrained
`gen.Gen.build`** (same operand model, correct-by-construction — never emits a
use-before-create / wrong-type / oversized operand), feeding the existing
`u3_run`. Seed-mutation (keep a seed, perturb its low bits) gives the feedback,
exactly as KFUZZ mutates its seeds.

The C generator must preserve every invariant U3's validator already enforces —
a bug here would turn into fuzzing NOISE, not a caught defect:

- resource-type compatibility (an `fd` handle only where an `fd` is wanted);
- create-before-use and the release lifecycle (no use-after-close unless
  deliberately tagged);
- operand bounds and non-negative ids/lengths;
- variant selector ↔ argp-shape compatibility (ioctl/arch_prctl);
- iovec segment limits (≤ `MAX_SEG`, each segment within its buffer).

Honesty tie-back (so the C generator can't silently drift from the Python model
we validated):

- every runtime-generated program is still under the U1/U2/U3 oracles — a
  generator bug that emits a malformed program surfaces as a worker fault, a
  leak, or a replay mismatch, never a silent pass;
- the worker, under a debug flag, prints the serialized IR of the programs it
  generates for a fixed seed set; a host selftest feeds those through the
  **existing Python validator**, so the C generator is checked against the exact
  same contract (incl. the invariants above) rather than trusted by assertion.

Lighter alternative (if the C generator is judged too big for the MVP): bake a
*bank* of N Python-generated programs and have the driver do coverage-guided
**selection/minimisation** over the bank. This reuses the Python generator with
zero new C, but it is weaker — it cannot generate *toward* an uncovered edge,
only pick among fixed candidates. Documented here as the fallback, not the plan.

## 5. Acceptance gate — and what measurement actually showed

The plan was to test "the feedback loop helps" *as such* via
`coverage_guided_beats_random`, with the MARGIN taken from a measured gap rather
than guessed. **We measured first, and the data overruled the headline claim** —
recorded here because the honesty of the result matters more than a green check.

**Measured (K=60, meta-seed fixed, guided vs random, repeated):**

```
u4: guided bits=33 (repeat 35) corpus=2 crashes=0 timeouts=0 | random bits=33..49
```

Two findings fall out, and both forbid an assertion the plan assumed:

1. **Seed-level feedback does NOT beat random here.** The generator is a pure
   function of the seed, so "mutating" a fruitful seed (`seed ^ rand`) produces
   an *unrelated* program, not a variant — the corpus carries no structural
   advantage, and guided vs random differ only by sample variance. A real
   feedback win needs **program-structure mutation** (tweak/splice a kept
   program), which is deferred (§8). So the gate must **never** assert
   `guided >= random`.
2. **The edge count is not bit-exact reproducible** (33 then 35/39 for the same
   meta-seed). Instrumented allocator/signal paths (`kheap`, `pmm`, `signal` are
   in `COV_SOURCES`) branch on evolving global state and IRQ timing. The seed
   sequence and the generated programs ARE deterministic; the resulting coverage
   *count* is not. So the gate must **never** assert an exact count or
   `g.bits == g2.bits`.

**What the gate therefore asserts (all robustly true, in `build-and-test`):**

- the campaign explores real coverage (`g.bits > 0`, `r.bits > 0`);
- a corpus of coverage-increasing seeds actually forms (`corpus_n > 0`);
- every generated program is clean under the runtime oracles — no uncontained
  fault, no hang (`crashes == 0`, `timeouts == 0`); the step-2b batch test adds
  the U2 net-zero-drift check over generated programs.

The guided/random/repeat numbers are **printed every run** so the gap (and any
future regression) is visible in the CI log even though it is not asserted. The
structural-authority gate (step 2a, `u4-gen-check`) remains the fail-closed
check that every generated program is well-formed; the fail-first for the
*feedback mechanism itself* belongs to a future structural-mutation brick, where
it can be asserted truthfully.

**Campaign metrics (printed every run, so we can tell real exploration from
"just ran more programs"):**

| Metric | Where | Why |
|---|---|---|
| new coverage edges (guided, repeat, random) | campaign KTEST, printed | exploration reached, and the guided/random gap + run-to-run spread |
| corpus size | campaign KTEST, printed + asserted `> 0` | did the seed corpus actually grow |
| crashes / timeouts | campaign KTEST, printed | uncontained faults / watchdog kills (U1) |
| OK count | campaign KTEST, asserted `== K` | every program ran, replayed and digest-matched (U1+U3; a replay/digest failure is a non-OK exit, so this subsumes "replay failures") |
| resource drift (pmm/kheap) | `u4_generated_programs_run_clean` (step 2b), asserted net-zero | leaks over a batch of generated programs (U2) |

(`programs/sec` was dropped — it adds a clock dependency and measures host speed,
not kernel behaviour; the fixed K already bounds the work.)

All four gates run in `build-and-test`. The campaign is a wall-clock budget, so
it is gated under `makh.notiming` in the stress multiplier (like the other
UFUZZ timing harnesses), and bounded to a fixed K (≈200 programs, KFUZZ-sized).

## 6. Instrumentation change

Add `kernel/syscall/syscall.c` and `kernel/fs/ext2.c` to `COV_SOURCES` (and
consider `kernel/proc/user.c`, `kernel/fs/devfs.c`, `kernel/mm/vmm.c`) so the
feedback sees **handler branch** coverage — the `ioctl`/`arch_prctl`/`fcntl`
switches, the ext2 paths — not only the callees. This is a small, reviewable
Makefile change; `-fsanitize-coverage=trace-pc` is already proven on the other
COV_SOURCES, so the only risk is build time, verified before landing.

## 7. Cost / CI budget

Per-program `fork+exec+wait` is heavier than KFUZZ's in-process targets, and v2
deliberately pays that cost (one program per worker) to keep attribution exact —
correctness over fork-thrift, per the review. K is therefore bounded modestly
(≈200 programs, KFUZZ-sized) so the campaign fits inside the build-and-test
budget; `proc.spawn_wait_many_no_leak` already shows many spawn/wait cycles run
comfortably there. If profiling later shows the fork cost dominates, the fix is
U4.1/Model B (in-process per-program coverage reads make batching safe again),
NOT reintroducing attribution-destroying batches here.

## 8. Explicitly out of scope for U4 MVP

- Multi-process / concurrent programs (a worker is still single-threaded).
- Structural splicing/crossover of typed programs (seed-mutation only).
- A persistent on-disk corpus (in-memory, per-campaign).
- Exact edge accounting — coverage is PC-hashed, so collisions undercount; it is
  a *signal*, consistent run-to-run, not a ground-truth edge set (KFUZZ accepts
  the same trade-off).
- Any claim that coverage implies correctness. U4 feeds the oracles; it is not
  itself an oracle.

## 9. Decisions (settled in review)

| Decision | Outcome |
|---|---|
| Model A (ring-0 driver + ring-3 workers) | **MVP** |
| Model B (gated ring-3 coverage syscall) | deferred to U4.1 |
| C runtime generator feeding `u3_run` | **yes** |
| Bake-a-bank selection | fallback only |
| Add `syscall.c` + `ext2.c` to `COV_SOURCES` | yes, after measuring build-time impact |
| Novelty signal | **edge identity** (`current & ~virgin`), per-seed, not a count |
| Batch per worker | **1 program** (attribution over fork-thrift) |
| Persistent / on-disk corpus | deferred |
| Concurrent / multi-process programs | deferred |
| `MARGIN`, K | from a MEASURED guided-vs-random gap, not guessed |

## 10. Pre-mortem — the failure we are designing against

The dangerous failure here is not a kernel crash; it is a **measurement lie**:

1. the driver reads only `cov_count()`;
2. a worker runs many programs in a batch;
3. coverage rises, but we cannot say which program caused it;
4. the corpus then keeps useless seeds and/or discards fruitful ones;
5. we announce "U4 is coverage-guided" while the feedback attribution is noise.

v2 is built to make that impossible: edge-identity novelty via the virgin map,
one program per worker so novelty binds to a single seed, an explicit
reset + warmup so lifecycle edges don't masquerade as discoveries, and the
`coverage_guided_beats_random` fail-first that collapses to random the moment
the keep-seed rule is removed. The target outcome: when UFUZZ reaches a new
syscall path, we know exactly which seed/program got it there — a corpus that
learns from execution, not a random syscall generator wearing a corpus.

## 11. Status at close — 2026-11-06 (resume point)

Branch `claude/inspiring-hawking-pgrf59`; everything below is pushed. CI verdicts
are GitHub's final conclusions, not local runs.

### Landed

| commit | what | CI |
|---|---|---|
| `bca97ec` | T-V01: read/write enforce `O_ACCMODE` (an `O_RDONLY` fd was writable) | #144 green |
| `7d338d4` | U4.1: `arch_prctl` in the generator; `base_seed`/`mut_seed` split | #145 green |
| `0a062a5` | P0: caps `U3_MAX_H=34`, `U3_MAX_CALLS=72`; over-cap programs fail loudly (0x500/0x501/0x502) instead of being silently truncated; `u3g_begin` bounds guard; `_Static_assert` invariants | #146 green |
| `ba97499` | a failed `open()` (`-EMFILE`) no longer truncates/creates — fail-first `vfs.failed_open_has_no_side_effect` | #147 **red** (stress: 1/12 timeout) |
| `74494c4` | `fd_install()`: atomic pick+publish at every free-slot site. `ba97499` had held an unpublished fd slot across ext2 create/truncate I/O while ring 0 is preemptible (`idt.c` IRQ tail) | — |
| `7ba2cdf` | stress prints each failed run's last `[ RUN ]` line | #148 green (stress 12/12) |
| `64b1040` | P1: ext2 block-map generator mode (direct/single/double-indirect tiers + a direct-only control), tier milestone, paired guided-vs-random | #149: abi-check + build-and-test green; stress still running when this was written — final verdict on GitHub |

### Measured — and what is NOT proven

- **ext2 tier milestone** (controlled): direct-tier saturation 0 new edges,
  direct-only noise floor 0, deep-tier programs 8, all 37 programs clean. The
  indirect tiers reach edges direct writes never do. *Not measured:* how those 8
  split between single- and double-indirect.
- **Guided vs random never produced a meaningful win:** arch_prctl selector
  mutation 13–19 vs 19–29; ext2 paired 16 vs 14 (guided ahead 2 of 3, one tie);
  the skeleton campaign swings from 13-vs-29 to 30-vs-20 between runs. Cause: the
  generator's surface (11 syscalls, 8-step valid-by-construction programs, one
  thread) saturates within a few runs, leaving nothing for feedback to find.
- **The #147 stress timeout was not reproduced:** the unfixed `ba97499` passed
  12/12 locally. The race `74494c4` closes is real by code reading, but that it
  caused that one CI timeout is unproven. If a stress timeout recurs, the
  `7ba2cdf` diagnostic names the test it hung in.
- **UFUZZ has not yet found a kernel bug on its own.** T-V01 was being exercised
  by generated programs (their writes to `O_RDONLY` fds succeeded) but no oracle
  flagged it — review did. The O_TRUNC defect and the fd race came from code
  reading. UFUZZ today is a validated generator with sound oracles, not a hunter.
  Lesson for the hunter: oracles must also catch *semantic* violations (a call
  that should fail but succeeds), not only crashes, hangs and leaks.

### Open items

1. **P2 EMFILE capacity** (not started; capacity already sized by P0): an
   `emfile` generator mode stacking 30 opens on a throwaway path then closing
   them; a KTEST asserting 29 distinct fds, the 30th `-EMFILE`, `OUT_OK`, and
   pmm/kheap net-zero; `validate.py` sweeps the new mode. A boundary test, never
   a guided-vs-random claim.
2. **Unverified side-finding:** `ext2_create` (`kernel/fs/ext2.c` ~766-767)
   returns NULL after a `write_inode`/`dir_add_entry` failure without freeing the
   inode allocated at ~733. Verify, then fail-first + fix.
3. **Known residuals:** an `open()` can still create/truncate if a sibling thread
   takes the *last* free slot during its create/truncate; `vfs_dup2` replaces an
   explicit slot non-atomically (pre-existing).

### Next, in order — turning UFUZZ into a hunter

- **P2** (small; infrastructure ready).
- **H1 hostile arguments:** keep the IR structurally valid but make values
  hostile — 0/-1/INT_MAX/huge or negative lengths; NULL, kernel-half, unmapped
  and read-only pointers; closed or out-of-range fds. Add semantic oracles
  (expected-error calls that succeed).
- **H2 `hunt` mode outside CI:** hours-long runs that save every crash/hang as
  seed + IR, dedupe by fault RIP/address, and auto-minimize the reproducer.
- **H3 concurrency:** `CLONE_FILES`/`CLONE_VM` threads in one program — the race
  class above.
- **H4** wider surface (pipe, dup2, fcntl, socket, fork, futex, rename,
  getdents); **H5** real program mutation (insert/delete/splice/operand tweak) —
  where guided feedback can finally have locality.

### Resume

- host authority (seconds): `make u4-gen-check`
- UFUZZ suite (~3 min): `make test-suite SUITE=ufuzz`
- full suite: `make all makhos-test.iso && python3 tools/run_tests.py makhos-test.iso --timeout 900`
  (`make test`'s 240 s cap is now too short for the full suite; use 900 s)
- stress: `make makhos-stress.iso && bash tools/stress_tests.sh 12 4 makhos-stress.iso`
