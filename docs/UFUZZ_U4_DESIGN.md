<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U4 Design Proposal: coverage-guided generation

**Status:** proposal for review (no code yet). Mirrors the U3 design-review flow.
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
The fork/exec/wait machinery itself adds a roughly-constant per-batch coverage
contribution; the *novelty delta* still isolates what the programs newly reach.
A single warmup batch before measurement absorbs the one-time spawn-path edges.

## 4. Generation strategy

The worker needs to turn a seed into a typed program **at runtime** (we can't
bake an unbounded set). Recommendation: a **C port of the constrained
`gen.Gen.build`** (same operand model, correct-by-construction — never emits a
use-before-create / wrong-type / oversized operand), feeding the existing
`u3_run`. Seed-mutation (keep a seed, perturb its low bits) gives the feedback,
exactly as KFUZZ mutates its seeds.

Honesty tie-back (so the C generator can't silently drift from the model we
validated in Python):

- every runtime-generated program is still subject to the U1/U2/U3 oracles — a
  generator bug that emits a malformed program shows up as a worker fault, a
  leak, or a replay mismatch, not as a silent pass;
- a host selftest serializes a sample of C-generated programs (the worker can
  print the IR under a debug flag) and runs them through the **existing Python
  validator**, so the C generator is checked against the same contract U3 uses.

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

All four run in `build-and-test`. The campaign is a wall-clock budget, so it is
gated under `makh.notiming` in the stress multiplier (like the other UFUZZ
timing harnesses), and bounded to a fixed K (≈200 iters, KFUZZ-sized).

## 6. Instrumentation change

Add `kernel/syscall/syscall.c` and `kernel/fs/ext2.c` to `COV_SOURCES` (and
consider `kernel/proc/user.c`, `kernel/fs/devfs.c`, `kernel/mm/vmm.c`) so the
feedback sees **handler branch** coverage — the `ioctl`/`arch_prctl`/`fcntl`
switches, the ext2 paths — not only the callees. This is a small, reviewable
Makefile change; `-fsanitize-coverage=trace-pc` is already proven on the other
COV_SOURCES, so the only risk is build time, verified before landing.

## 7. Cost / CI budget

Per-iteration `fork+exec+wait` is far heavier than KFUZZ's in-process targets,
so the worker runs a **batch** of programs per spawn (`/bin/ufuzz cov <seed0>
<count>`) to amortise it, and the driver reads coverage per batch. K and the
batch size are tuned so the campaign fits well inside the build-and-test budget
(the U3 campaigns already run comfortably there).

## 8. Explicitly out of scope for U4 MVP

- Multi-process / concurrent programs (a worker is still single-threaded).
- Structural splicing/crossover of typed programs (seed-mutation only).
- A persistent on-disk corpus (in-memory, per-campaign).
- Exact edge accounting — coverage is PC-hashed, so collisions undercount; it is
  a *signal*, consistent run-to-run, not a ground-truth edge set (KFUZZ accepts
  the same trade-off).
- Any claim that coverage implies correctness. U4 feeds the oracles; it is not
  itself an oracle.

## 9. Open questions for review

1. Model A vs B for the MVP (recommendation: A; B as U4.1).
2. C runtime generator vs bake-a-bank selection (recommendation: C generator).
3. Which files to add to `COV_SOURCES` (minimum: syscall.c, ext2.c).
4. The `MARGIN` and K for `coverage_guided_beats_random` — pick values that are
   comfortably above run-to-run noise, justified by a measured guided-vs-random
   gap rather than guessed.
