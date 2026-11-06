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

## 5. Acceptance gate (evidence-first, fail-first)

The headline claim — "the feedback loop helps" — must be tested *as such*, not
implied by "a fuzzer ran":

1. **`coverage_guided_beats_random`**: run K iterations with the keep-seed-on-
   new-coverage rule ON, and K with it OFF (always-fresh seeds), same meta-seed
   and budget. Assert `guided_edges >= random_edges + MARGIN`. **Fail-first:**
   deleting the single "keep the seed" line collapses guided into random and the
   assertion fails — the test proves the *mechanism*, not just execution.
2. **Determinism**: a fixed meta-seed yields identical final coverage across
   boots (so CI is reproducible and a regression is attributable).
3. **Regression**: the U4 campaign covers at least the edge count U3's baked set
   reaches (U4 never explores *less* than the fixed set it subsumes).
4. **Oracle integration**: every program the campaign runs is still under U1/U2
   (no uncontained fault, net-zero drift after the campaign settles).

The MARGIN is not guessed: the campaign first PRINTS the guided-vs-random gap
(and the table below), and MARGIN is set comfortably below the *measured* gap.
If measurement shows guided does not convincingly beat random on MAKH's small
syscall surface in a short campaign, the honest response is to weaken the claim
(e.g. "guided reaches the same coverage in fewer programs", or a reproducible
non-trivial corpus) rather than ship a flaky `>=` — U4 asserts only what the
numbers actually support.

**Campaign metrics (printed every run, so we can tell real exploration from
"just ran more programs"):**

| Metric | Why |
|---|---|
| unique coverage bits (`popcount(virgin)`) | how much kernel was reached |
| executed programs | work done |
| programs/sec | execution efficiency (fork cost is real) |
| corpus size | did the seed corpus actually grow |
| crashes (139) | uncontained faults (U1) |
| timeouts (137) | hangs the watchdog killed (U1) |
| resource drift (pmm/kheap) | leaks over the campaign (U2) |
| replay failures | non-deterministic results (U3) |

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
