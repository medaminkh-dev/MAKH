# MakhOS Phase 15: KFUZZ — a ring-0, coverage-guided self-fuzzer

**Version:** 0.1.0-dev
**Status:** COMPLETE — the kernel fuzzes itself; 11 KFUZZ tests, sandbox proven to recover from a real ring-0 fault, 20/20 stress runs clean
**Depends on:** Phases 12–14 (scheduler, pthreads, networking) and their invariant oracles

---

## Table of Contents

1. [The Idea](#1-the-idea)
2. [The Ring-0 Sandbox](#2-the-ring-0-sandbox)
3. [Coverage Feedback](#3-coverage-feedback)
4. [The PRNG and Reproducibility](#4-the-prng-and-reproducibility)
5. [The Targets](#5-the-targets)
6. [Invariant Oracles](#6-invariant-oracles)
7. [The Watchdog](#7-the-watchdog)
8. [Driving It: Shell and Tests](#8-driving-it-shell-and-tests)
9. [Build Integration](#9-build-integration)
10. [What It Found and What It Proves](#10-what-it-found-and-what-it-proves)

---

## 1. The Idea

A fuzzer feeds a program random input and watches for crashes. Doing that to a
kernel usually means an external harness (syzkaller) driving it from user space
through syscalls. MakhOS does it **from the inside, in ring 0**: a sandboxed
kernel thread generates random inputs and calls the kernel's own functions —
the heap allocator, the physical page allocator, `memmove`, the pthreads API,
the shell parser, and the network receive path — directly, at full speed, with
no privilege boundary in the way.

The hard part is that a bad input in ring 0 normally kills the machine. KFUZZ
makes faults **survivable and reproducible**: every crash is caught, tagged
with the exact seed that produced it, and the campaign continues.

```
MakhOS> fuzz 5000
fuzzing 5000 iterations...
done: 5000 iters, 0 crashes, 0 oracle-fails, coverage 2731 edges, corpus 38
```

---

## 2. The Ring-0 Sandbox

The core is a `setjmp`/`longjmp` pair written in assembly
(`kernel/kfuzz/jmp.asm`) — the freestanding kernel has no libc.

Running one iteration:

```c
int jumped = kfuzz_setjmp(sb.jb);   // save callee-saved regs, rsp, return addr
if (jumped) {
    // arrived here via a fault -> longjmp; sb.fault_* describe it
    if (sb.cleanup) sb.cleanup();   // drop any lock the target held
    log("CRASH seed=... vector=... replay: fuzz replay ...");
    return CRASH;
}
sb.active = 1;
target->fn(&rng, iters);            // may fault anywhere, arbitrarily deep
sb.active = 0;
```

When a CPU exception (vectors 0–31) fires while `sb.active`, the exception
handler in `kernel/arch/idt.c` hands control to the sandbox instead of
panicking:

```c
if (vector < 32 && kfuzz_in_sandbox())
    kfuzz_report_fault(regs);       // records vector/rip/cr2, then longjmp
```

`kfuzz_report_fault()` records the fault and `kfuzz_longjmp()`s back to the
harness. Because the fault was taken through an interrupt gate (which clears
IF), `kfuzz_longjmp` ends with `sti` so interrupts are live again in the
harness.

**Locks across a fault.** A target that holds a real mutex (only `netrx`, which
takes `net_lock` around `net_input`) registers a `cleanup` callback. On
recovery the harness calls it to release the lock — otherwise the next
iteration would deadlock. Everything else the fuzzer touches is guarded only by
IRQ-off critical sections, which `longjmp`'s `sti` already restores.

This is validated, not assumed: `kfuzz.sandbox_recovers_from_page_fault` runs a
target that deliberately writes to an unmapped canonical address, and asserts
the `#PF` is caught (`rc == 1`) and the kernel is fully functional afterwards
(allocate, walk the heap, run a clean campaign).

---

## 3. Coverage Feedback

The subject files — `kheap.c`, `pmm.c`, `string.c`, `shell.c`, and the whole
`net/` stack — are compiled with `-fsanitize-coverage=trace-pc`, so GCC emits a
call to `__sanitizer_cov_trace_pc()` at every basic block. KFUZZ implements that
callback:

```c
void __sanitizer_cov_trace_pc(void) {
    if (!cov_enabled) return;
    uint64_t pc = (uint64_t)__builtin_return_address(0);
    uint32_t idx = (uint32_t)((pc * GOLDEN) >> 48) & (COV_BITS - 1);
    cov_map[idx >> 6] |= 1ull << (idx & 63);     // set a bit; idempotent
}
```

Setting a bit is idempotent, so no lock is needed — a rare lost update under
preemption only under-counts, it never corrupts. The **harness** (`kfuzz.c`,
`kfuzz_targets.c`) is *not* instrumented, so the map measures the code under
test, not the fuzzer.

**Corpus.** A seed that raises the edge count is kept (up to 64). Each iteration
either mutates a corpus seed or draws a fresh one, so inputs that reach new code
are favoured — coverage-guided fuzzing in miniature.

Only the subject subsystems are instrumented (not arch/sched/irq), keeping the
hook off the hot interrupt paths and out of the fuzzer itself.

---

## 4. The PRNG and Reproducibility

`xorshift64*` — three shifts and a multiply, seeded per iteration:

```c
uint64_t kfuzz_rand(kfuzz_rng_t* r) {
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}
```

Every iteration is driven entirely by one 64-bit seed, so a crash is exactly
reproducible:

```
MakhOS> fuzz replay 0x609a5f1aa29f9058 netrx
replay seed=0x609a5f1aa29f9058 target=netrx -> clean
```

The crash log prints the replay command for you.

---

## 5. The Targets

| Target | What it does | Local oracle (beyond the global ones) |
|---|---|---|
| `heap` | random `kmalloc`/`kfree`/`krealloc`/`kcalloc` over 48 live slots, each stamped with a tag byte | payload survives across other ops; `krealloc` preserves the prefix; `kcalloc` zeroes |
| `pmm` | random page alloc/free over 32 slots | every page is page-aligned and never handed out twice while live |
| `string` | random `memcpy`/`memmove`/`memset`/`strlen`/`strchr` with 16-byte guard bands | `memmove` matches a naive reference even on overlap; guards never overwritten |
| `pthread` | create/join/detach threads, mutex trylock/lock, semaphore post/wait | (global oracles: tree + heap consistency) |
| `shell` | feeds `shell_exec()` structured commands and raw random bytes | the only requirement — it must return, never crash |
| `netrx` | feeds `net_input(lo, …)` random frames and bit-flipped/truncated valid Ethernet+IPv4+TCP frames | the RX path never faults or corrupts state on malformed input |
| `fault` | *(not in `all`)* deliberately faults, to test the sandbox itself | — |

Every target is bounded (fixed live-set sizes, capped lengths) and cleans up
after itself, so a clean run leaves the heap at its starting size — the tests
assert exactly that.

---

## 6. Invariant Oracles

After **every** target run, three oracles must hold, or the seed is reported as
an `ORACLE FAIL` (a silent-corruption bug — no fault, but the kernel's
invariants broke):

1. **`kheap_check()`** — every heap block's magic/size/alignment/footer, the
   free list, and the `heap_used` accounting are consistent.
2. **`kheap_get_bad_frees()`** — unchanged (no double/wild free slipped through).
3. **`proc_tree_check()`** — the process tree's parent/child links are intact.

These are the same oracles built during the Phase 14 bug hunt, now doing
permanent duty.

---

## 7. The Watchdog

A target can hang instead of crash — an infinite loop, or a deadlock. A
high-priority watchdog thread reads a heartbeat the campaign bumps each
iteration; if it stops advancing for more than 4 seconds while a target is
active, the watchdog panics with the culprit seed:

```
*** KERNEL PANIC ***
KFUZZ watchdog: target 'shell' hung on seed 0x609a5f1aa29f9058
```

This is not hypothetical — it fired the first time the `shell` target was
allowed to run `ping 10.0.2.2`, which blocks for seconds on ARP/echo timeouts.
That was a real "input takes too long" finding; the fix was to keep unreachable
gateway IPs out of the shell target's vocabulary.

---

## 8. Driving It: Shell and Tests

**Shell:**

```
fuzz                       # 2000 iterations across every target
fuzz heap 5000             # one target, N iterations
fuzz replay <seed> <tgt>   # deterministically reproduce one seed
```

**CI smoke (`make test`):** `kernel/tests/test_kfuzz.c`

- PRNG determinism + seed sensitivity, `rand_below` range (incl. `n == 0`).
- `setjmp`/`longjmp` round-trip.
- **Sandbox recovers from a real page fault** and the kernel survives.
- Each of the six targets: a 400-iteration campaign with **zero crashes, zero
  oracle failures, no leak**.
- A 1500-iteration all-target campaign that must build **> 100 coverage edges**
  and a non-empty corpus — proving the feedback loop actually works.

**Interactive smoke (`make smoke`):** `tools/shell_smoke.py` types
`fuzz string 300` and `fuzz netrx 300` through the emulated keyboard and checks
for `0 crashes`.

---

## 9. Build Integration

```makefile
COV_SOURCES = kernel/mm/kheap.c kernel/mm/pmm.c kernel/lib/string.c \
              kernel/shell/shell.c kernel/net/*.c
COV_OBJECTS = $(COV_SOURCES:.c=.o)
$(COV_OBJECTS): CFLAGS += -fsanitize-coverage=trace-pc
```

Only the subject files carry the instrumentation flag (a GNU make
target-specific variable), so arch, scheduler and the fuzzer itself compile
normally. `kernel/kfuzz/` adds `kfuzz.c`, `kfuzz_targets.c` and `jmp.asm`.

> Note: toggling the coverage flag does not change the `.c` timestamps, so
> after adding/removing a file from `COV_SOURCES` do a `make clean` (or delete
> the affected `.o`) to force a rebuild. A normal clean build always applies it.

---

## 10. What It Found and What It Proves

- The **watchdog** immediately caught a target whose input caused multi-second
  blocking (`ping` to an unreachable host).
- The heap, pmm, string, pthread, shell and netrx targets run **clean** over
  hundreds of thousands of iterations across the test and stress runs — strong
  evidence that the Phase 11–14 subsystems handle random and malformed input
  without faulting, leaking, or breaking their invariants.
- The **sandbox recovery is proven** by a test that injects a genuine ring-0
  page fault and shows the kernel surviving it.

The self-fuzzer is now permanent infrastructure: every `make test` runs it, and
`make stress` runs the whole thing 20+ times in parallel. New subsystems get
fuzzed for free by adding a target and, if they have integrity invariants,
wiring them into `oracles_ok()`.
