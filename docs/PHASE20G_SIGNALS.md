# MakhOS Phase 20-G: user signal handlers (`sigaction`/`sigreturn`)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a ring-3 process can install a signal handler; the kernel
builds a signal frame on the user stack, runs the handler, and `sigreturn`
resumes the interrupted code. 146 in-kernel tests
**Depends on:** Phase 19 (pending/blocked masks, dispositions), Phase 20-A
(preemptible user processes, the trapframe), Phase 20-F (job control; this is
the last piece of a real signal story)

---

## 1. Scope

Phase 19 built the signal *subsystem* (pending sets, `sigprocmask`, default
actions) and Phase 20-D delivered the default **terminate** action to a running
process (that is Ctrl+C). What was missing is the thing a shell, a REPL, or musl
actually needs: **a user-installed handler**. This brick adds it.

- **`sigaction(sig, handler)`** — install a C function as the handler for `sig`
  (or `SIG_DFL`/`SIG_IGN`);
- **delivery** — on the way back to ring 3 from a syscall, if a handler is
  pending the kernel saves the interrupted context on the user stack, masks the
  signal, and redirects execution to `handler(signo)`;
- **`sigreturn`** — when the handler returns, a tiny user trampoline calls
  `sigreturn`, which restores the saved context and resumes exactly where the
  process was;
- **`sigprocmask`** — already existed in the kernel; now wired to ring 3, so a
  process can block a signal across a critical section and have it delivered on
  unblock.

**Deferred:** delivery on the *timer-IRQ* path (a purely CPU-bound process that
installs a handler and never makes a syscall won't run it until its next
syscall — the IRQ path uses a different register-frame layout and only applies
the default terminate action); `SA_SIGINFO`/`siginfo_t`, `SA_RESTART`,
alternate signal stacks (`sigaltstack`), and real-time signal queueing.

---

## 2. The delivery mechanism (`signal.c`, `syscall.c`)

Delivery happens at the **syscall-return** boundary, where the kernel holds the
process's `trapframe_t` and the syscall epilogue will `sysret` straight from it.
That is the one place a handler frame can be built cleanly, so `syscall_dispatch`
calls `signal_deliver(tf)` on every return:

```
user stack (high -> low), built by signal_deliver:

   ... interrupted frame ...
   [ 128-byte red zone, skipped ]
   [ sigcontext ]  r15..rax, rip, rflags, rsp, saved blocked-mask   <- 16-aligned
   [ return addr = sa_restorer ]                                    <- handler's RSP
                                                                        (%16 == 8)
```

`signal_deliver` picks the lowest-numbered pending, unblocked signal; if a
handler is installed it saves the full interrupted context into the
`sigcontext`, writes the trampoline address just below it as the handler's
return address, masks the signal, and rewrites the trapframe: `rip = handler`,
`rsp` = the return-address slot, `rdi = signo` (the handler's argument). The
epilogue then `sysret`s into the handler. If there is **no** handler, the old
behaviour applies — terminate on a fatal default (Ctrl+C), discard on a benign
one.

The IRQ path (`idt.c`) still calls `signal_check_and_die()`, which now **skips**
any signal that has a handler (it cannot build the frame from the IRQ's
`registers_t`), leaving it pending for the next `signal_deliver`.

## 3. `sigreturn` (`signal.c`, `start.S`)

The handler is an ordinary `void handler(int)`. When it returns, it `ret`s to
the kernel-supplied **trampoline** (`__sigreturn_trampoline` in `start.S`,
handed to the kernel as `sa_restorer` by the `usignal` wrapper, so the kernel
never needs to know a user symbol address). At that point `rsp` points exactly
at the `sigcontext`, and the trampoline issues `rt_sigreturn`:
`signal_sigreturn` copies the context back into the trapframe and the process
resumes at the original `rip`/`rsp` with its registers intact. `RFLAGS` is
sanitised on the way back — only the user arithmetic/direction flags survive,
`IF` is forced on, and IOPL can never be raised from ring 3 — so a forged frame
cannot escalate.

## 4. Lifetime across `fork`/`execve` (`proc/user.c`)

- **`fork`** copies the handler table and the restorer, so a child keeps its
  parent's handlers (POSIX).
- **`execve`** calls `signal_reset_handlers`: caught handlers point into the old
  image, so they revert to `SIG_DFL` (an `SIG_IGN` disposition survives, per
  POSIX).

## 5. Fuzzed

No new KFUZZ target: the delivery and restore paths are driven entirely by the
trapframe and a bounded, fixed-size `sigcontext`, with every user access going
through `copy_to_user`/`copy_from_user` (a bad or forged stack returns `-EFAULT`
and the process is killed with SIGSEGV rather than corrupting the kernel). The
adversarial surface — an unmapped user stack at delivery, and a clobbered or
hostile `sigcontext` at `sigreturn` (including the RFLAGS sanitiser) — is
exercised directly by the acceptance programs and the existing `signal` target
covers the pending/mask bookkeeping.

## 6. Tests (`kernel/tests/test_usignal.c`) + programs (`user/`)

Each program encodes the whole result in its exit status, so the kernel just
reads it back with `waitpid`:

- **`usignal.handler_runs_and_sigreturn_resumes`** (`/bin/sigtest`) — installs a
  SIGTERM handler, raises it, and exits **42**: the status proves the handler
  ran *and* `sigreturn` resumed the mainline (a broken frame would crash or
  return 0).
- **`usignal.sigprocmask_defers_delivery`** (`/bin/sigmask`) — blocks SIGTERM,
  raises it (stays pending), runs a mainline step, then unblocks. The decimal
  sequence resolves to **12**, proving the signal waited while blocked and was
  delivered on unblock.

**146 in-kernel tests pass**; `make stress` (serial) is clean.

## 7. Deferred

| Item | Why it waits |
|---|---|
| IRQ-path handler delivery | the IRQ `registers_t` differs from the syscall `trapframe_t`; a frame-builder for it is its own change |
| `SA_SIGINFO` / `siginfo_t` | handlers currently take only `signo` |
| `SA_RESTART` | interrupted syscalls return `-EINTR`; auto-restart is extra |
| `sigaltstack`, RT-signal queueing | not needed until a libc wants them |

With user handlers in place, **gap #1 (the process model) is complete**: a
process can fork/exec, own the terminal, parse arguments, and catch signals —
the foundation a musl/busybox userland stands on. The next gaps are storage
(virtio-blk + a real FS), the wider syscall surface (`arch_prctl` for TLS,
`pipe`/`dup`/`stat`/`getdents`), and kernel spinlocks.
