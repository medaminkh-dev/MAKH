# UFUZZ U3 — Typed Syscall Program Model (design proposal)

**Status: PROPOSAL for review. No code yet.** This answers the five design
questions for U3 with concrete MAKH examples, grounded in the U0 ABI inventory
(`fuzz/abi/makh_syscalls.json`). We review and agree this before implementing.

The goal of U3 is to replace U1's fixed lifecycle menu with programs that are
*structurally valid and semantically adversarial*: sequences of real syscalls
with tracked relationships between them, replayable byte-for-byte.

---

## 1. How is a syscall program represented?

A program is a typed IR (an ordered list of calls over a small operand model),
not a blob of random bytes. Three parts:

```
Program {
  abi_version : u16          # must equal makh_syscalls.json abi_version
  prng        : {algo, seed} # for regeneration/attribution
  buffers     : [Buffer]     # a pool: id -> bytes (the data mmap/write/… point at)
  calls       : [Call]       # the ordered sequence
}

Call {
  sys       : u16            # syscall number, taken from the U0 inventory
  args      : [Operand]      # one per ABI argument, in ABI order
  binds     : ResourceHandle?# if this call creates a resource (per U0 resources.creates)
}

Operand =
  | Imm(i64)                 # a scalar immediate (flags, lengths, modes, …)
  | Res(handle)              # a reference to a resource produced by an earlier call
  | Buf(buffer_id, offset)   # a pointer argument -> into the buffer pool
  | Path(path_id)            # a path string (its own small interned pool)
```

Every `Operand`'s *kind* is dictated by the matching U0 argument descriptor:
a `scalar` arg becomes `Imm` (or `Res` when it carries `res:` — e.g. an fd is an
`int` scalar that names a resource), a `ptr` arg becomes `Buf`/`Path` sized by
the arg's `size` rule. So the IR cannot express an argument shape the ABI does
not describe.

Serialization: a compact versioned binary form (magic + abi_version + prng +
buffer table + call table). Text form for diffs/debug. The envelope is the one
U8/U6 will reuse for the corpus and crash artifacts.

---

## 2. How are dependencies between fd / pid / addr / buffer tracked?

Via a **typed symbol table of resource handles**, one namespace per U0 resource
type (`fd`, `pid`, `tid`, `addr`, `socket`, `pipe_end`, `path`, `buffer`,
`signal_context`).

- When a call's U0 entry lists `resources.creates: [fd]` (e.g. `open`, nr 2),
  the generator allocates a fresh handle `%fd_k` and records
  `binds = %fd_k` on that call.
- An argument whose U0 descriptor has `res: fd` is emitted as `Res(%fd_k)`
  referring to a **live** handle of that type.
- `resources.releases: [fd]` (e.g. `close`, nr 3) moves the referenced handle
  to the RELEASED state.
- `pipe` (nr 22) binds **two** `pipe_end` handles (its U0 `creates` lists two);
  `fork` binds a `pid`; `mmap` binds an `addr`; `munmap` releases it.

A dependency edge is therefore `(consumer call, producer handle)`, recovered
directly from the `Res` operands. The buffer pool gives the data-dependency
edges for `Buf` operands.

---

## 3. How is use-before-create / use-after-release prevented?

Two layers:

- **Generation invariant.** The generator only emits `Res(h)` for a handle `h`
  that is currently LIVE in the symbol table (created by an earlier call, not
  yet released). It never references a handle that does not exist yet. Buffers
  referenced by a `Buf` operand must exist in the pool and be large enough for
  the arg's `size` rule.
- **Deliberate violations are tagged, never accidental.** Adversarial cases —
  use-after-close, double-close, use-before-create — are produced *only* by a
  named mutation operator that sets a `mutation` tag on the call (so triage and
  the oracle know the violation is intended). The baseline generator never
  emits them silently.

A standalone **IR validator** (shared by generator, mutator, and replay)
re-checks every program: each `Res` points to a handle created earlier; it is
LIVE unless the call carries the matching `after_release`/`before_create`
mutation tag; each `Buf` fits its buffer; each `Imm`/`Res`/`Buf` matches the
arg kind the U0 entry requires. An invalid program that is not explicitly
tagged is a generator bug, and the validator fails the build/test.

---

## 4. How is deterministic replay ensured?

The program is **fully self-contained**: every immediate, every buffer byte,
every path, and the PRNG algo+seed are serialized. Replay = deserialize +
execute the calls in order with exactly those operands. Nothing is drawn from
the environment at replay time.

Honest limit (spec §3.1): replay reproduces the **program**, not the
**schedule**. Single-process programs replay deterministically. Timing- or
scheduling-sensitive outcomes (a futex race, a signal-delivery window) may not
reproduce every run; U3 programs record no wall-clock dependence, and such
cases carry a reproduction-rate rather than a determinism claim (that is U7/U9
territory). The IR itself is deterministic; the kernel's scheduling is not
promised to be.

---

## 5. How does the program bind to the U0 ABI inventory?

`makh_syscalls.json` is the single input that parameterizes the generator,
mutator, and validator. For each syscall the generator reads:

- `nr` → the `Call.sys` value;
- each arg's `kind` → whether to emit `Imm`, `Res`, or `Buf`/`Path`;
- each `ptr` arg's `size` rule → how big a buffer to allocate and bind
  (`len:ARG` ties the buffer length to another operand; `struct:NAME` and
  `fixed:N` give fixed sizes; `cstr`/`cstr_arr` drive string generation);
- `dir` → whether the buffer needs meaningful input bytes (`in`/`inout`) or is
  an output sink (`out`);
- `resources` → the create/consume/release edges for the symbol table;
- `impl` → `unsupported`/`stub` syscalls are skipped or de-weighted (no point
  fuzzing a constant stub); `partial` entries carry the per-arg notes from the
  semantic audit (e.g. `rt_sigaction` is `(sig, handler, restorer)`, not a
  struct — the generator must emit code addresses, not a sigaction buffer).

This binding is also the guard rail: because the generator is driven by the
inventory, and U0's `check_abi.py` keeps the inventory consistent with the
kernel, U3 cannot drift from the real ABI without a visible CI failure.

---

## Worked example (the use-after-close case)

ChatGPT's example, expressed in the IR (handles shown as `%fdN`, buffers as
`#bufN`). Syscall numbers and resource edges are the real ones from U0:

```
buffers:
  #buf0 = <64 random bytes>
  path0 = "/mnt/created.txt"

calls:
  c0: open(Path(path0), Imm(O_CREAT|O_RDWR))   binds %fd0     # U0: open nr 2, creates fd
  c1: write(Res(%fd0), Buf(#buf0,0), Imm(64))                 # U0: write nr 1; buf dir=in, size len:count
  c2: close(Res(%fd0))                                        # U0: close nr 3, releases %fd0
  c3: write(Res(%fd0), Buf(#buf0,0), Imm(64))                 # mutation: after_release(%fd0)  <-- intended UAC
```

- c1's `Buf(#buf0,0)` with `Imm(64)` satisfies write's `buf (in len:count)`
  from U0 — buffer length is tied to the `count` operand.
- c2 moves `%fd0` to RELEASED.
- c3 is a **tagged** use-after-release; the validator permits it only because
  of the `after_release` tag, and triage knows the EBADF (or worse) it expects
  is intended, not a harness bug.

A buffer/mapping example, again with real U0 shapes:

```
calls:
  m0: mmap(Imm(0), Imm(4096), Imm(PROT_READ|PROT_WRITE),
           Imm(MAP_PRIVATE|MAP_ANON))          binds %addr0   # U0: mmap nr 9, addr scalar(hint), creates addr
  m1: mprotect(Res(%addr0), Imm(4096), Imm(PROT_READ))        # U0: mprotect nr 10, addr scalar res:addr
  m2: munmap(Res(%addr0), Imm(4096))                          # U0: munmap nr 11, releases %addr0
```

Note the audit's effect: `mmap`/`mprotect`/`munmap` `addr` are modelled as
`Res`/`Imm` address operands (`res: addr`), **not** as data buffers — so the
generator will not waste effort filling a "buffer" the kernel never reads.

---

## Open questions for review (before coding)

1. **IR encoding**: compact binary (fast, opaque) vs. a line-based text IR
   (diff-friendly, trivially replayable). Proposal: text IR first (U3 values
   clarity over throughput), binary later if the executor needs it.
2. **Where does the executor live?** The U1 `/bin/ufuzz` worker gains an "run
   this serialized program" mode (Ring 3), reading the program from a known
   source. Proposal: start by compiling a batch of generated programs *into*
   the worker binary for the first KTEST, then add external delivery (a file /
   shared page) once the format is stable — avoids a delivery channel before
   the IR is proven.
3. **Generator placement**: host-side (Python, alongside `check_abi.py`,
   emitting programs the worker replays) vs. in-guest. Proposal: host-side
   generator + validator in `fuzz/` driven by the JSON, so it is testable in CI
   without booting; the guest only *executes*.
4. **Acceptance gates** (from spec §U3): measure the fraction of programs that
   reach their intended syscall; assert the validator rejects an untagged
   use-before-create; prove a program round-trips through
   serialize→deserialize unchanged. Agree these as the U3 fail-first set.
