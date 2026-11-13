# UFUZZ U3 — Typed Syscall Program Model (design proposal, v2)

**Status: PROPOSAL for review. No code yet.** v2 revises v1 after review. The
direction (typed, dependency-tracked, replayable programs) is accepted; this
revision answers four gaps: a richer operand model, precise resource lifecycle,
a replay *trace* (not just the program), and — the key one — pushing
conditional/nested shapes into the **U0 ABI schema** rather than hiding an ABI
gap inside U3. It ends with the smallest MVP, per-feature acceptance tests, and
an explicit deferred list.

Guiding rule (from the review): U3 does **not** solve all of fuzzing in v1. The
MVP generates a valid program, tracks its resources, runs it in Ring 3, and
replays it. Complexity is added incrementally after that is proven.

---

## 1. Operand model (v2)

v1's four kinds (`Imm`/`Res`/`Buf`/`Path`) cannot express optional pointers,
conditional argument shapes, nested pointers, structs, or arrays. The revised
model is **concrete and nestable**:

```
Operand =
  | Imm(i64)                 # scalar immediate (flags, lengths, modes, ids)
  | Res(handle)              # reference to a LIVE resource handle (fd/addr/…)
  | Null                     # explicit NULL, for an optional pointer
  | Buf(buffer_id)           # pointer to a flat byte buffer in the pool
  | Path(path_id)            # interned NUL-terminated path string
  | Struct(struct_id)        # pointer to a typed struct instance; each FIELD is
                             #   itself an Operand (this gives nested pointers)
  | Array(elem_kind, [Operand])  # pointer to N elements, each an Operand
```

Nesting is the point: an `iovec` is `Struct{ iov_base: Buf(#b0), iov_len:
Imm(n0) }`; a `writev` iov argument is `Array(iovec, [Struct{…}, Struct{…}])`.
A struct field that is a `Buf`/`Res`/`Null` **is** a nested pointer.

**Conditionality is resolved at generation time into a concrete operand.** The
IR never carries an unresolved "it depends"; `ioctl`'s `argp` is already a
concrete `Buf`/`Struct` chosen to match the concrete `request` immediate in the
same call. What must *know* about the dependence is the generator — and that
knowledge lives in the ABI schema (§2), not in a special runtime operand. This
keeps replay exact (no re-evaluation) while still modelling the dependence
honestly.

Pools: `buffers` (flat bytes), `structs` (typed instances with per-field
operands), `paths` (interned strings). All are serialized with the program.

---

## 2. U0 ABI schema evolution (the review's point #4)

Rather than let U3 improvise around an incomplete inventory, extend
`makh_syscalls.json` so the shapes U3 needs are *described*, and
`check_abi.py` keeps them honest. Proposed additions, with real MAKH examples:

**(a) Optional pointers** — `optional: true` on args that may be NULL. Real
cases: `rt_sigprocmask.oldset`, `nanosleep.rem`, `prlimit64.old_limit`,
`futex.timeout`.

```json
{"name":"rem","kind":"ptr","ctype":"struct timespec*","dir":"out",
 "size":"struct:timespec","optional":true}
```
IR may then emit `Null` or `Buf` for it; the generator exercises both.

**(b) Conditional argument shape** — `variants`, a selector arg plus a map from
selector value → concrete shape. Real cases:

`ioctl.argp`, keyed by `request` (evidence: `do_ioctl` switch on request):
```json
{"name":"argp","kind":"variant","selector":"request","variants":{
   "TCGETS":   {"kind":"ptr","dir":"out","size":"struct:termios"},
   "TCSETS":   {"kind":"ptr","dir":"in","size":"struct:termios"},
   "TIOCGPGRP":{"kind":"ptr","dir":"out","size":"fixed:4"},
   "TIOCSPGRP":{"kind":"ptr","dir":"in","size":"fixed:4"}}}
```
`arch_prctl.addr`, keyed by `code` (evidence: `do_arch_prctl` switch on code):
```json
{"name":"addr","kind":"variant","selector":"code","variants":{
   "ARCH_SET_FS":{"kind":"scalar"},
   "ARCH_GET_FS":{"kind":"ptr","dir":"out","size":"fixed:8"}}}
```
The generator picks the `request`/`code` immediate first, then emits `argp`/
`addr` per the matching variant. `check_abi.py` gains: every `variant` arg has a
`selector` naming another arg, and each variant is a valid arg shape.

**(c) Struct layouts with nested pointers** — `struct_defs`, referenced by the
`struct:NAME` size rule, so the model knows a struct field is itself a pointer:
```json
"struct_defs": {
  "iovec": {"size":16, "fields":[
    {"name":"iov_base","kind":"ptr","dir":"inout","size":"len:iov_len"},
    {"name":"iov_len","kind":"scalar","ctype":"size_t"}]}
}
```
`readv.iov`/`writev.iov` then declare `size:"arr:iovcnt:iovec"` (already do) and
the generator knows each element carries a nested `iov_base` pointer whose
direction follows the call (read→out buffers, write→in buffers).

These are **proposals to apply as the first step of U3**, with `check_abi.py`
extended to validate them. No schema change ships without the checker enforcing
it — the audit discipline (`docs/UFUZZ_ABI_AUDIT.md`) continues to apply.

---

## 3. Resource lifecycle (the review's point #2)

v1 treated handles as created unconditionally. v2 adds a **runtime value layer**
and success-gating. A handle has a static identity (`%fd0`) and a runtime record
filled by the executor: `{ value, state }` where state ∈ {PENDING, LIVE,
FAILED, RELEASED}.

- **Success-gating.** A `creates` call binds a handle as PENDING. After
  execution the executor sets it LIVE with the real value only on success
  (`fd ≥ 0`; `mmap != MAP_FAILED`; `pipe == 0`), else FAILED. A later `Res(h)`
  of a FAILED handle is *not* a generator bug — it is a defined test of the
  syscall's handling of a bad fd/addr — but the executor records that the
  reference resolved to a failed resource.
- **mmap → MAP_FAILED.** No valid `addr` handle is produced; `%addr0` goes
  FAILED. A following `munmap(Res(%addr0), …)` runs against `-1`, exercising
  munmap's bad-address path without crashing the controller. The generator
  prefers args that succeed (so the intended mapping is reached), but the model
  is correct when they don't.
- **pipe → two ends to the real integers.** `pipe` binds `%pe0,%pe1`; the
  executor reads back `pipefd[0]`/`pipefd[1]` from the out-buffer and sets
  `%pe0.value = pipefd[0]`, `%pe1.value = pipefd[1]`. Subsequent
  `read(Res(%pe0))`/`write(Res(%pe1))` resolve to those actual fds.
- **Partial / overlapping munmap.** An `addr` handle carries `[base,len]`. Full
  release (munmap of the whole mapping) is the MVP; sub-range munmap that splits
  a mapping is modelled in the type but **deferred** (a tagged operation later).
- **fork/clone → execution paths, not a PID handle.** fork returns pid (parent)
  / 0 (child): two control-flow paths, shared/copied resources, and ordering.
  This is **deferred** — U3 MVP is single-process (spec U3: multi-process "when
  the interface is ready"); the path-graph IR is specified with U9 (concurrency),
  not forced into the MVP.

Validator (static, shared by generator/mutator/replay): every `Res(h)` refers to
a handle created earlier in program order; it is in a usable static state
(created, not statically RELEASED) unless the call carries the matching
`after_release`/`before_create` **mutation tag**; every `Buf`/`Struct`/`Array`
satisfies its arg's size rule; every operand kind matches the (possibly
variant-resolved) ABI descriptor. An untagged violation fails the build.

---

## 4. Replay (the review's point #3)

The program alone reproduces the *inputs*; to know *why* a result differs we also
record an **execution trace** next to it:

```
Trace {
  worker_pid : i32
  calls : [ { ret:i64, errno:i32, bound:{handle->value},
              out_digest:{buf_id->hash}, seq:u32 } ]   # per call, in order
  verdict : { reproducible | timing_sensitive }         # see below
}
```

- per call: return value, errno, the concrete values bound to any handles
  (actual fd/addr/pipe integers), and a digest of each out/inout buffer after
  the call;
- `worker_pid` (process identity) and a logical `seq` for ordering, so an
  async event (a signal) has a place in the order;
- replay re-runs the program and diffs the new trace against the recorded one;
  the first differing call localizes the divergence.

**Reproducible program vs. non-deterministic scheduling** stays explicit: a
single-process program with no timing dependence must reproduce its trace
exactly (asserted). A program whose outcome depends on scheduling (a futex race,
a signal-delivery window) records a reproduction rate over N attempts and is
marked `timing_sensitive` — never a false determinism claim (spec §3.1). The MVP
records ret+errno+bound-values and asserts exact single-process reproducibility;
full buffer-digest diffing feeds U7 (minimization) and is deferred there.

---

## 5. IR examples for the hard cases

`ioctl` (conditional argp resolved against request):
```
c0: open(Path("/dev/tty"?), Imm(O_RDWR))            binds %fd0
c1: ioctl(Res(%fd0), Imm(TCGETS), Struct(#t0:termios[out]))   # argp = out termios, per variant
```

`arch_prctl` (conditional addr resolved against code):
```
c0: arch_prctl(Imm(ARCH_SET_FS), Imm(0x7ffff000))   # addr is a scalar value here
c1: arch_prctl(Imm(ARCH_GET_FS), Buf(#b0, size=8))  # addr is an out pointer here
```

`futex` (optional timeout):
```
c0: futex(Buf(#w0,size=4), Imm(FUTEX_WAIT), Imm(0), Null)        # no timeout
c1: futex(Buf(#w0,size=4), Imm(FUTEX_WAIT), Imm(0), Struct(#ts0:timespec))  # with timeout
```

`writev` (array of struct with nested in-pointers):
```
c0: open(Path("/mnt/f"), Imm(O_CREAT|O_WRONLY))     binds %fd0
c1: writev(Res(%fd0), Array(iovec, [
        Struct{ iov_base: Buf(#b0), iov_len: Imm(16) },
        Struct{ iov_base: Buf(#b1), iov_len: Imm(32) } ]), Imm(2))
```

`readv` is identical but each `iov_base` is an **out** buffer (direction flows
from the call), showing why the struct_def field direction must follow context.

`pipe` (two ends bound to real integers) + data round-trip:
```
c0: pipe(Buf(#fds, size=8))     binds %pe0=fds[0], %pe1=fds[1]   # filled at runtime
c1: write(Res(%pe1), Buf(#b0), Imm(8))
c2: read (Res(%pe0), Buf(#b1,size=8), Imm(8))
c3: close(Res(%pe0));  c4: close(Res(%pe1))
```

`mmap` failure path:
```
c0: mmap(Imm(0), Imm(0xFFFFFFFFF000), Imm(PROT_READ), Imm(MAP_PRIVATE|MAP_ANON)) binds %addr0
    # runtime: returns MAP_FAILED -> %addr0 = FAILED
c1: munmap(Res(%addr0), Imm(4096))   # runs against -1; munmap's bad-addr path, no crash
```

---

## 6. U3 MVP (smallest viable scope)

Included:
- **Single-process** programs only.
- **Operands**: `Imm`, `Res`, `Null`, `Buf`, `Path`. (`Struct`/`Array`/`variant`
  designed in §1–2 but their *generation* is the next increment — see deferred.)
- **Resources**: `fd` (open/openat/dup2/pipe/close), `addr`
  (mmap/munmap full-range), `path`. Runtime value layer + success-gating.
- **Executor**: `/bin/ufuzz` gains a "replay serialized program" mode (Ring 3),
  reusing U1's fork-isolation + watchdog.
- **Generator + validator**: host-side Python in `fuzz/`, driven by the U0 JSON,
  so it runs in CI without booting; the guest only executes.
- **Serialization**: text IR first (diff-friendly, trivially replayable).
- A small set of **hand-specified adversarial templates** (double-close,
  use-after-close), tagged — enough to prove the validator accepts intended
  violations. The general mutation engine is U8.

The MVP's syscall coverage is every call whose args are Imm/Res/Null/Buf/Path —
the large majority (open/close/read/write/lseek/stat/mkdir/unlink/mmap/munmap/
mprotect/getpid/chdir/getcwd/dup2/pipe/getrandom/nanosleep/…).

---

## 7. Acceptance tests (per feature; fail-first where it applies)

1. **Generation validity**: ≥ an agreed fraction of generated programs reach
   their intended final syscall (measured + reported, not assumed).
2. **Validator rejects bad programs** (fail-first): feed it an *untagged*
   use-before-create and an *untagged* use-after-release → both rejected;
   the same programs with the mutation tag → accepted.
3. **Success-gating**: a program whose `mmap` is forced to fail (huge length)
   binds no LIVE `addr`; the following `munmap` runs the failure path and the
   controller stays up (reuses U1 containment).
4. **pipe round-trip**: a generated pipe/write/read/close program moves the
   bytes through the actual `pipefd` integers (data read back == data written).
5. **Serialize round-trip**: program → serialize → deserialize → re-serialize is
   byte-identical.
6. **Replay determinism**: a single-process program reproduces its ret+errno
   (+bound-values) trace across N runs; divergence fails.
7. **Containment** (reuse U1): a generated program that faults is contained and
   the campaign continues.

Each becomes a CI gate (host-side generator/validator tests run without QEMU;
executor/replay tests run as KTESTs like `test_ufuzz.c`).

---

## 8. Deferred (explicit, so nothing hides)

- **Struct/Array/variant operand generation** (readv/writev/iovec, ioctl,
  arch_prctl) → next U3 increment (U3.1), after the MVP proves the pipeline. The
  schema (§2) lands with the MVP so it is ready.
- **Multi-process / multi-thread** programs (fork/clone, parent/child path
  graph, shared resources) → U9 (concurrency); the path-graph IR is specified
  there, not in U3 MVP.
- **Partial/overlapping munmap, mapping splits** → later U3.
- **General mutation engine** (bit/flag/length/reorder/aliasing, broad tagged
  violations) → U8; U3 MVP ships only a few fixed adversarial templates.
- **Coverage-guided generation / feedback** → U4.
- **Full buffer-state before/after diffing + minimization** → U7.
- **Snapshot/persistent execution** → later (gated on reset correctness, U2).

---

## 9. Open questions for this review

1. Agree the MVP scope in §6 (single-process, 5 operand kinds, fd/addr/path)?
2. Agree to land the §2 schema additions (optional/variant/struct_defs) *with*
   the MVP — described and checker-enforced — even though the MVP doesn't yet
   *generate* struct/array/variant operands?
3. Agree the acceptance set in §7 as the U3 fail-first gates?
4. Text IR first (vs. binary) — acceptable for the MVP?

No implementation begins until these are settled.
