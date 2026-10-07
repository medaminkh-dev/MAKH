# MakhOS Phase 20-L: user threads — `clone` + `futex` + `set_tid_address`

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a ring-3 process can spawn a thread that shares its
address space and fds, synchronise with a futex, and be joined. 154 in-kernel
tests
**Depends on:** Phase 20-A-2 (fork, the trapframe return path), Phase 20-H
(per-thread FS base), Phase 20-A (wait queues, the scheduler)

---

## 1. Scope

Brick 1.4 of Stage 1 and the last prerequisite for musl's pthreads. It turns the
one-thread-per-process model into one that supports **multiple threads in a
shared address space**, with the three primitives a libc thread rests on:

- **`clone`** — create a thread that shares the caller's address space
  (`CLONE_VM`), fd table (`CLONE_FILES`) and signal dispositions, runs on a
  caller-supplied stack with its own TLS, and reports its tid;
- **`futex`** — `FUTEX_WAIT`/`FUTEX_WAKE`, the kernel side of every pthread
  mutex, condvar and the join handshake;
- **`set_tid_address`** + `CLONE_CHILD_CLEARTID` — the thread's tid word is
  cleared and futex-woken on exit, which is exactly how `pthread_join` returns.

**Deferred:** full `CLONE_THREAD` thread-group semantics (a signal sent to the
process waking an arbitrary thread; a shared pending-signal set; `tgkill`;
`gettid` vs `getpid` distinction) — each thread is still its own schedulable
entity with its own pid. That is enough for pthreads; the group niceties come
with musl bring-up.

---

## 2. Shared address space, refcounted (`mm/vmspace.c`, `sched.c`)

The model change: an `address_space_t` now carries a **refcount**. `clone(CLONE_VM)`
points the new thread's `aspace` at the parent's and bumps it; `reap` tears a
space down only when the **last** thread referencing it is reaped (otherwise it
just decrements). Without this, the first thread to exit would free the memory
out from under its siblings. The fd table is shared the same way under
`CLONE_FILES`: a small refcount (`fd_rc`) means `vfs_close_all` only closes and
frees the table when the last sharer exits.

## 3. `clone` (`proc/user.c`)

`proc_clone` mirrors `fork`, with the difference that defines a thread: it does
**not** copy the address space — it shares it (refcount++) — and it starts the
child on the caller-supplied `child_stack` with `rax = 0`, reusing the
`fork_child_entry` iretq return path. `CLONE_SETTLS` seeds the thread's FS base
(its TLS); `CLONE_CHILD_SETTID` writes the new tid into the user word now, and
`CLONE_CHILD_CLEARTID` records it to clear on exit. A cloned thread is created
**detached** — it is joined through the futex, not `waitpid`, so it auto-reaps
on exit (which also drops the address-space refcount promptly). `clone` without
`CLONE_VM` is just `fork`.

The user side is a small `__clone_thread` trampoline in `start.S`: it stashes
`fn`/`arg` on the child stack, issues the syscall, and in the child (`rax == 0`)
pops them and calls `fn(arg)`, then `exit(0)` — which fires CLEARTID.

## 4. `futex` (`futex.c`)

A hashed-bucket futex keyed by the **physical** address of the word (so threads
sharing a page land on the same bucket, and identical virtual addresses in
different spaces don't collide):

- **`FUTEX_WAIT(uaddr, val)`** — if `*uaddr != val`, return `-EAGAIN` (the fast
  path already changed); otherwise block on the bucket until woken, with an
  optional timeout (`-ETIMEDOUT`) and signal interruption (`-EINTR`).
- **`FUTEX_WAKE(uaddr, n)`** — wake up to `n` waiters.

The check-then-sleep is made atomic against a concurrent wake by holding
interrupts off across both on this single-CPU kernel — nothing can run in
between, so no wakeup is lost. Bucket hash collisions only cause spurious
wakeups, which the futex contract allows and the caller re-checks.

## 5. `pthread_join` via CLEARTID (`sched.c`)

When a thread exits, `thread_exit` — while its address space is still live —
writes 0 to its `clear_child_tid` word and `futex_wake`s it. A joiner spins
`while (tid_word != 0) futex_wait(&tid_word, tid_word)`; the clear+wake releases
it. This is precisely musl's `__pthread_join` handshake.

## 6. Fuzzed

No new KFUZZ target: a futex `WAIT` **blocks**, which the single-threaded
sandbox cannot drive without a partner to wake it, and `clone`'s surface is PCB
and refcount bookkeeping rather than untrusted-byte parsing. The adversarial
cases — `FUTEX_WAIT` value mismatch (`-EAGAIN`), a bad word address
(`-EFAULT`), signal/timeout wakeups, and the shared-aspace/fd lifetime across
thread exit — are exercised by the acceptance program and guarded by the
refcount chokepoints (`reap`, `vfs_close_all`).

## 7. Tests (`kernel/tests/test_thread.c`) + program (`user/threadtest.c`)

- **`thread.clone_futex_mutex_and_join`** (`/bin/threadtest`) — a worker thread
  and `main` each increment a shared counter 20000 times under a futex mutex;
  `main` then joins via the CLEARTID futex. A final counter of exactly 40000
  (no lost updates → mutual exclusion held) plus a clean join give exit **55**.
  The program proves, in one shot: `clone` sharing memory, the futex mutex, and
  join.

**154 in-kernel tests pass**; `make stress` (serial) is clean.

## 8. Deferred

| Item | Why it waits |
|---|---|
| full `CLONE_THREAD` group semantics | per-process signal delivery to any thread; `tgkill`/`gettid` |
| `FUTEX_REQUEUE`, `FUTEX_WAIT_BITSET`, PI futexes | not needed by basic pthreads |
| `set_robust_list` | robust-mutex cleanup on crash |

With threads, the TLS base (20-H) and the syscall surface (20-I/J/K), the
prerequisites for a **musl port** are in place. The remaining Stage-1 item is
the wall clock (1.5, RTC); after that comes F20 — building musl against this
ABI and booting `busybox sh`.
