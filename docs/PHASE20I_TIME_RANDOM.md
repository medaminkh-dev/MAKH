# MakhOS Phase 20-I: time & randomness syscalls

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — ring 3 can read the clock, sleep, and draw random bytes:
`clock_gettime`, `gettimeofday`, `nanosleep`, `getrandom`. 149 in-kernel tests
**Depends on:** Phase 20-A (preemptible processes, wait queues), Phase 19
(signal-interruptible waits)

---

## 1. Scope

This is the first brick of **Stage 1 — rounding out the syscall surface a libc
expects** (the lead-up to a musl port). The kernel already *had* monotonic time
(`ktime.c`) but nothing in ring 3 could reach it, and there was no randomness
source at all. Both are things a C runtime touches on startup (malloc
hardening, stack canaries, `time()`), so they come first.

- **`clock_gettime(clk, ts)`** — monotonic (and, for now, realtime-aliased) time;
- **`gettimeofday(tv, tz)`** — the same, in `timeval` form (tz ignored);
- **`nanosleep(req, rem)`** — sleep, **interruptible** by a signal (`-EINTR` with
  the remainder written back);
- **`getrandom(buf, len, flags)`** — fill a buffer from a kernel PRNG.

**Deferred:** a real wall clock (`CLOCK_REALTIME` still aliases monotonic until
the RTC brick, 1.5); `clock_nanosleep`/`TIMER_ABSTIME`; a blocking entropy pool
(`getrandom` never blocks — `GRND_*` flags are accepted and ignored).

---

## 2. Time (`syscall.c`, `ktime.c`)

`clock_gettime` and `gettimeofday` are thin wrappers over `ktime`'s
tick-derived `clock_now_ms()` (10 ms resolution at 100 Hz), copied out with
`copy_to_user`. Both `CLOCK_REALTIME` and `CLOCK_MONOTONIC` resolve to the
monotonic value for now — honest, and 1.5 (RTC) will give realtime a true epoch
without touching callers.

`nanosleep` is the interesting one: it must wake on a **signal**, not just its
timeout. It parks on a private wait queue that nothing ever wakes
(`sleep_wq`), with the requested duration as the timeout, and reads
`sched_wait_event`'s verdict: a return of 2 means a signal interrupted the sleep,
so it computes the unslept remainder, writes it to `rem`, and returns `-EINTR`;
otherwise it slept the whole span and returns 0. A zero request returns
immediately (a zero timeout would otherwise mean "wait forever").

## 3. Randomness (`krandom.c`, `syscall.c`)

A small kernel PRNG backs `getrandom`:

- **xoshiro256\*\*** generates the stream (a modern, well-distributed generator);
- **splitmix64** spreads a single seed word across the 256-bit state;
- the state is **seeded lazily from the TSC** (`rdtsc`) on first use, and each
  draw folds in a *fresh* `rdtsc()` — so the output is not a pure function of the
  boot-time seed, a cheap moving-entropy twist;
- the state mutation runs with interrupts off, so a timer preemption can't
  interleave two draws and bias the stream.

It is **not crypto-grade** (no real entropy pool yet), but it is well-mixed and
non-repeating — enough for canaries, ASLR jitter and malloc hardening. The
design idea is borrowed from the standard generators and adapted (the per-draw
TSC remix and the irq-guard are ours); nothing is copied wholesale.

## 4. Fuzzed

No new KFUZZ target: the handlers are bounded `copy_to/from_user` plus arithmetic
on a single generator, and the adversarial cases — a bad user pointer
(`-EFAULT`), a malformed `timespec` (`-EINVAL`), a signal mid-sleep (`-EINTR`) —
are on the acceptance paths. The RNG's non-repetition is checked directly in
kernel space.

## 5. Tests (`kernel/tests/test_time.c`) + program (`user/timetest.c`)

- **`time.clock_nanosleep_getrandom_from_userspace`** (`/bin/timetest`) — from
  ring 3: `clock_gettime(MONOTONIC)` straddling a 30 ms `nanosleep` shows the
  clock advanced, and two `getrandom` draws are non-zero and differ; exits **77**.
- **`time.krandom_draws_are_distinct`** — 256 kernel-side `krandom_u64()` draws
  with no collision (a stuck or zero generator would repeat at once).

**149 in-kernel tests pass**; `make stress` (serial) is clean.

## 6. Deferred

| Item | Why it waits |
|---|---|
| true `CLOCK_REALTIME` (epoch) | needs the CMOS RTC — brick 1.5 |
| `clock_nanosleep`, `TIMER_ABSTIME` | absolute deadlines; relative sleep suffices now |
| blocking entropy / `/dev/random` | needs a real entropy pool; the PRNG never blocks |

Next in Stage 1: **`stat`/`fstat`/`getdents64` + `fcntl`** (1.2), so programs can
see file metadata and list directories.
