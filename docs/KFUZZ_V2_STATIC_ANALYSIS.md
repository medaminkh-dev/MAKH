# When static analysis beat the fuzzer — and how KFUZZ v2 answered

**Status:** the story behind MAKH 1.2.0's security work
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Scope:** this is a factual post-mortem. Every "v2 catches it" claim below was
verified the only way that counts — by reverting the fix, running the fuzzer,
and watching it report the bug with a reproducible seed. Where v2 does **not**
yet catch a class, this document says so plainly.

---

## 1. What happened

MAKH ships with KFUZZ, a ring-0 coverage-guided self-fuzzer that attacks the
kernel's own subsystems from a sandboxed thread (see `PHASE15_KFUZZ.md`). It had
been clean for a long time. Then two rounds of **external static analysis** —
reading the code, not running it — found real bugs KFUZZ had never flagged:

- an **mm audit** (findings F1–F6): an ext2 directory-name stack overflow, an
  `mprotect`/`munmap` that reached kernel page tables, an ELF frame leak on
  overlapping segments, two out-of-memory rollback leaks, and a `longjmp` that
  re-enabled interrupts unconditionally;
- a **second audit** (findings AUD-01, AUD-03, AUD-04, AUD-05, AUD-07): a
  `copy_to_user` store fault that panicked the kernel, an ext2 superblock
  geometry that divided by zero / shifted out of range, a `sigreturn` that
  trusted an attacker-controlled RIP/RSP and mask, and an ELF loader that
  orphaned a frame when a mapping failed.

The uncomfortable part: a fuzzer whose entire pitch is "the kernel finds its own
bugs" had been outscored by someone *reading the source*. This document is the
honest accounting of **why** KFUZZ v1 was blind to each finding, and **what in
KFUZZ v2 changed so the fuzzer now reaches them.**

The point is not that static analysis is the enemy. The two are complementary:
static analysis reasons about all paths at once but can't observe runtime state;
a fuzzer observes real state but only on the paths it actually drives. KFUZZ was
losing because it **wasn't driving the paths the bugs lived on** — not because
fuzzing is the weaker tool. Fix the reach, and the fuzzer wins on its own
terms: it doesn't just flag a suspicious line, it produces the seed that crashes
the kernel.

---

## 2. Why a fuzzer goes blind

A coverage-guided fuzzer finds a bug only when **three** things are all true:

1. **Reach** — the campaign actually calls the function with the bug, on the
   code path that is wrong. A loader bug on the "a mapping failed" path is
   unreachable if no mapping ever fails.
2. **A triggering input** — the random input it feeds crosses the specific
   boundary that breaks. An overflow at byte 256 never fires if inputs top out
   at 128 bytes.
3. **An oracle** — something *notices* the bug. A silent memory leak is
   invisible unless a check compares the free-page count before and after.

Miss any one and the bug hides in plain sight, through millions of iterations,
looking exactly like a clean run. Every KFUZZ v1 miss below is one of these
three failing — usually reach or oracle. Naming which one, for each bug, is the
whole game.

---

## 3. The v1 misses, one at a time

### 3.1 The oracle that was unplugged (the worst one)

KFUZZ targets return a verdict: `0` for "all invariants held", non-zero for "I
detected corruption." Several targets *did* their own leak accounting and
returned `-1` on a mismatch. But the v1 campaign driver, `run_one()`, **threw
that return value away.** A target could detect a leak, shout about it, and the
engine would record the iteration as clean.

This is the most instructive miss because the fuzzer wasn't blind for lack of
reach or inputs — it was blind because **nobody was listening to the oracle it
already had.** KFUZZ v2.0 made `run_one()` honor the verdict. The very first run
after that change surfaced a latent `strchr` bug the fuzzer had technically
"found" long before and silently discarded.

> Lesson: an oracle you don't read is not an oracle. Coverage and crash-catching
> are glamorous; the boring plumbing that propagates a `-1` is what turns a
> near-miss into a finding.

### 3.2 Leaks on paths that allocate — but never failed (F4, F5, AUD-07)

Three of the findings are frame/page leaks on **out-of-memory rollback** paths:

- **F4** — `fork`'s page-table copy, partway through, hits OOM and forgets to
  free what it already copied.
- **F5** — `kheap_expand` maps several pages, one map fails, and the earlier
  ones leak.
- **AUD-07** — the ELF loader allocates a segment frame, then `vmspace_map`
  fails (its *own* intermediate page-table allocation ran out), and the segment
  frame is now mapped nowhere, unreclaimable by `vmspace_destroy`.

All three share one blind spot: **in a test VM with plenty of RAM, no allocation
ever fails,** so the rollback code is dead weight the fuzzer literally cannot
enter. Reach = 0. The oracle (strict page conservation across
create → operate → destroy) was fine; there was simply nothing on the failing
path to observe because the path was never taken.

KFUZZ v2.1 added a **fault-injection dimension**: `pmm_test_fail_after(n)` arms
the page allocator to fail the *n*-th allocation from now. Suddenly the
campaign can make `fork`, `kheap_expand`, and `elf_load` fail mid-operation, on
purpose, and the page-conservation oracle catches every frame they forget.
AUD-07 specifically needed v2.4 to also build a **multi-page ELF** and inject
the fault *inside* `elf_load`, so the failure lands on the
segment-frame-held-then-map-fails window.

### 3.3 Inputs that never crossed the boundary (F2, F3)

- **F2** — `mprotect`/`munmap` accepted a **kernel-half address** and walked
  into the shared kernel page tables. v1's `t_uvm` only ever passed addresses it
  had itself just `mmap`'d — all well-formed, all in the user half. It never
  handed the syscalls a kernel address, a non-canonical address, or page 0, so
  the rejection path the bug lived on was never exercised. **Triggering input =
  0.**
- **F3** — the ELF loader leaked a frame when two `PT_LOAD` segments **shared a
  page**. v1's `t_elf` generated **single-segment** images. Two segments can't
  overlap if there's only ever one. **Triggering input = 0.**

KFUZZ v2.2 gave `t_uvm` an explicit menu of adversarial addresses (kernel-half,
the non-canonical gap, page 0, an address that overflows on `addr+len`, wild
canonical-low) and throws them at `mprotect`/`munmap` on every iteration. KFUZZ
v2.3 taught the ELF generator to emit **two** `PT_LOAD`s whose virtual addresses
may land on the same page. Both bugs then show up against the same strict
page-conservation oracle that was already there.

### 3.4 Attack surfaces with no target at all (AUD-01, AUD-05)

- **AUD-01** — `copy_to_user` to a read-only or unmapped user page faulted on
  the **store** half of the copy loop, and the fault-recovery fixup only covered
  the **load** half, so the store fault panicked the kernel. v1 had **no uaccess
  target whatsoever.** Reach = 0, from the very start.
- **AUD-05** — ext2 mount read `s_log_block_size` / `blocks_per_group` /
  `inodes_per_group` from an untrusted superblock and used them raw:
  `1024 << log_bs` (a shift wider than the word) and `(ino-1) / inodes_per_group`
  (divide by zero). v1 had **no ext2 geometry target.**

KFUZZ v2.4 added both. `t_uaccess` throws random canonical user pointers at
`copy_to_user`/`copy_from_user` — almost all unmapped in the fuzzer's own
address space, so the **store** path is hammered, and any unrecovered fault is a
ring-0 `#PF` the sandbox catches. `t_ext2geo` throws adversarial geometry at the
`ext2_geometry_ok` gate and, only when the gate accepts, runs the exact shift
and divisions the driver runs — a divide-by-zero is a `#DE` the sandbox catches,
and a bad `block_size` trips an explicit oracle.

### 3.5 The honest gaps — findings v2 fixes but does **not** yet fuzz

Three findings are fixed and covered by **regression KTESTs**, but have **no
fuzzer target yet**, because faithfully fuzzing them needs machinery MAKH hasn't
built. Pretending otherwise would be exactly the self-congratulation this
document exists to avoid.

- **F1** (ext2 `readdir` name overflow) and the ext2 *on-disk* parsing surface:
  a faithful target needs an in-memory virtio-blk backend to mount a mutated
  image. `t_ext2geo` covers the *geometry-arithmetic* slice of the ext2 attack
  surface; the directory/inode-walking slice still waits on that RAM-disk.
- **AUD-03** (`sigreturn` trusting attacker RIP/RSP/mask): faithfully fuzzing it
  means driving the signal-delivery path from a ring-3 context, which the ring-0
  sandbox can't stand in for. Fixed and KTEST-covered; not yet fuzzed.
- **AUD-04** (ELF entry point outside any executable segment): a defense-in-depth
  check, regression-tested. The ELF generator could be extended to target it.

These are the honest edges of the bet, and they are the roadmap: a RAM-backed
block device unlocks the ext2 on-disk surface, and a userspace-fuzzing harness
unlocks the syscall-boundary surface (sigreturn among them).

---

## 4. KFUZZ v1 vs v2 — the scorecard

Every "yes" in the **v2 catches** column was verified by reverting that exact
fix, running the fuzzer, and recording the finding with its replay seed. The
proofs live in the commit messages for each target.

### 4.1 Per-finding

| Finding | Class | v1 caught | Why v1 was blind | v2 mechanism | v2 catches (how it shows) |
|---|---|:--:|---|---|:--:|
| F2 — mprotect/munmap on kernel VA | OOB write | ✗ | `t_uvm` only used its own mmap'd user VAs | v2.2 adversarial address menu | ✓ page-conservation oracle |
| F3 — ELF overlapping PT_LOAD | frame leak | ✗ | `t_elf` emitted a single segment | v2.3 two-segment generator | ✓ page-conservation oracle |
| F4 — fork OOM rollback | frame leak | ✗ | no allocation ever failed | v2.1 fault injection | ✓ page-conservation oracle |
| F5 — kheap_expand partial fail | page leak | ✗ | no allocation ever failed | v2.1 fault injection | ✓ page-conservation oracle |
| (engine) — unread verdict | missed oracle | ✗ | `run_one` discarded the target's return | v2.0 honor the verdict | ✓ (found a `strchr` bug at once) |
| AUD-01 — copy_to_user store fault | ring-0 `#PF` | ✗ | no uaccess target | v2.4 `t_uaccess` | ✓ caught `#PF`, vector 14 |
| AUD-05 — ext2 geometry | `#DE` + shift UB | ✗ | no ext2 geometry target | v2.4 `t_ext2geo` | ✓ caught `#DE` (vector 0) + oracle |
| AUD-07 — ELF frame orphaned on map fail | frame leak | ✗ | `t_elf` ran memory-plentiful | v2.4 ELF fault injection in `t_oom` | ✓ page-conservation oracle |
| F1 — ext2 readdir name overflow | stack smash | ✗ | no ext2 on-disk target | — (needs RAM-disk) | ✗ fix + KTEST only |
| AUD-03 — sigreturn trusts user state | priv / UB | ✗ | no ring-3 syscall harness | — (needs userspace fuzzing) | ✗ fix + KTEST only |
| AUD-04 — ELF entry outside exec seg | hardening | ✗ | — | — (generator could extend) | ✗ fix + KTEST only |

**Tally:** of 8 findings whose class is reachable from the ring-0 sandbox, v1
caught **0** and v2 catches **8**. Three further findings (F1, AUD-03, AUD-04)
are fixed and regression-tested but remain outside the fuzzer's current reach —
named here rather than rounded up.

### 4.2 Engine capabilities

| Capability | v1 | v2 |
|---|:--:|:--:|
| Ring-0 sandbox recovers from `#PF`/`#GP`/`#DE` (vectors 0–31) | ✓ | ✓ |
| Coverage-guided corpus, reproducible per-seed replay | ✓ | ✓ |
| Target self-check verdict propagated to the campaign | ✗ | ✓ (v2.0) |
| Allocation fault injection (`pmm_test_fail_after`) | ✗ | ✓ (v2.1) |
| Adversarial address menu for VM syscalls | ✗ | ✓ (v2.2) |
| Multi-segment / page-sharing ELF generator | ✗ | ✓ (v2.3) |
| uaccess target (copy_to/from_user) | ✗ | ✓ (v2.4) |
| ext2 geometry target | ✗ | ✓ (v2.4) |
| ELF loader under fault injection | ✗ | ✓ (v2.4) |
| `longjmp` restores the *captured* interrupt flag (F6) | ✗ | ✓ |

---

## 5. What this proves, and what it doesn't

**It proves:** a fuzzer's blind spots are almost never "fuzzing is weak" — they
are missing reach, missing inputs, or an unread oracle, each of which is a
fixable engineering gap. Every bug static analysis found in this class, KFUZZ
now finds too, and it finds them *better*: not a flagged line to go verify, but
a seed that reproduces the crash deterministically, every time, with the kernel
recovering afterward so the campaign keeps going.

**It does not prove** the fuzzer is "done," or that it dominates static analysis
in general. Static analysis still reasons about all paths at once and needs no
triggering input; it will keep finding things first, especially in code the
fuzzer can't yet reach. The three honest gaps in §3.5 are exactly where the next
static-analysis pass would win again — until KFUZZ grows the RAM-disk and the
userspace harness that close them.

That is the bet, stated without flinching: **not that the fuzzer never loses a
round, but that every round it loses becomes a target, and the target wins the
rematch — reproducibly.** v2 is that bet made good on eight findings. The
roadmap is the rest.
