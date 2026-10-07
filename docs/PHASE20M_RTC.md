# MakhOS Phase 20-M: the wall clock (CMOS RTC)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `CLOCK_REALTIME` and `gettimeofday` return a real Unix
epoch, anchored to the CMOS RTC at boot. 156 in-kernel tests. **Stage 1 (the
syscall surface for a libc) is complete.**
**Depends on:** Phase 20-I (the time syscalls this now backs), the PIT timer

---

## 1. Scope

Brick 1.5 of Stage 1, and the last of it. Phase 20-I exposed `clock_gettime`,
but `CLOCK_REALTIME` only aliased the monotonic tick counter — there was no true
date. This reads the motherboard clock once at boot so the wall clock is real:
file timestamps, `time()`, build stamps and TLS/entropy seeding all get a
genuine epoch.

- **`rtc_read_epoch()`** — read the CMOS RTC and return seconds since 1970 (UTC);
- **`CLOCK_REALTIME` / `gettimeofday`** — boot epoch + monotonic offset;
- **`CLOCK_MONOTONIC`** — unchanged (ticks since boot).

**Deferred:** RTC update interrupts / periodic resync (we read once and advance
with the monotonic timer — no NTP, so there is a little drift), per-process
time zones (the clock is UTC), and `settimeofday`/`clock_settime`.

---

## 2. Reading the CMOS clock (`drivers/rtc.c`)

The RTC lives behind CMOS ports 0x70/0x71. Two hazards, both handled:

- **mid-tick reads** — the chip may be updating when sampled, so we wait for the
  "update in progress" flag to clear and then read the fields **twice**,
  accepting them only when two reads agree. No half-ticked value is ever
  latched.
- **encoding** — status register B says whether the fields are BCD or binary and
  whether hours are 12- or 24-hour; both are decoded (BCD unpacked, PM folded
  into 24h).

The civil date is converted to a Unix timestamp with the standard
days-from-civil formula (adapted), giving UTC seconds since 1970. (The CMOS year
is two digits, taken as 20xx.)

## 3. Anchoring the wall clock (`ktime.c`, `kernel.c`)

`ktime_init_realtime()` runs once right after the timer starts: it captures the
RTC epoch and the current monotonic offset and stores `realtime_base_ms` =
epoch − monotonic. From then on `clock_now_realtime_ms()` =
`realtime_base_ms + clock_now_ms()`, so realtime advances with the same cheap
tick counter as monotonic — no CMOS access on the hot path. `clock_gettime`
selects the base by clock id; `gettimeofday` uses realtime.

## 4. Fuzzed

No new KFUZZ target: `rtc_read_epoch` reads fixed hardware registers (no
untrusted input), and the arithmetic is exercised at every boot — a wrong
decode would put the epoch far outside the sane window the tests assert. The
field-settling loop is the one piece of defensive logic and is covered by the
agree-twice invariant.

## 5. Tests (`kernel/tests/test_rtc.c`) + program (`user/clktest.c`)

- **`rtc.realtime_epoch_is_sane`** — a direct kernel check: the realtime epoch
  is after 2020 and vastly greater than the seconds-since-boot monotonic value
  (so the boot epoch is actually applied, not a zero base).
- **`rtc.wall_clock_from_userspace`** (`/bin/clktest`) — from ring 3:
  `CLOCK_REALTIME` is a post-2020 epoch far ahead of `CLOCK_MONOTONIC`,
  `gettimeofday` agrees to within a couple of seconds, and the clock advances
  across a 30 ms sleep. Exit **66**.

**156 in-kernel tests pass**; `make stress` (serial) is clean.

## 6. Stage 1 complete

With the wall clock in, Stage 1 — rounding out the syscall surface a C library
expects — is done:

| Brick | Gave userland |
|---|---|
| 20-H | `arch_prctl(SET_FS)` — the TLS thread pointer |
| 20-I | `clock_gettime`/`nanosleep`/`getrandom` |
| 20-J | `stat`/`fstat`/`getdents64`/`fcntl` |
| 20-K | `pipe`/`dup2`, fd inheritance, shell pipelines |
| 20-L | `clone`/`futex`/`set_tid_address` — threads |
| 20-M | the RTC wall clock |

That is the ABI musl stands on. Next is **F20**: build musl static against this
surface, run a standard C program, then boot `busybox sh`.

## 7. Deferred

| Item | Why it waits |
|---|---|
| RTC update IRQ / periodic resync | one read + monotonic advance is enough; drift is tiny |
| `settimeofday` / `clock_settime` | nothing needs to set the clock yet |
| time zones | the kernel clock is UTC; zones are a libc concern |
