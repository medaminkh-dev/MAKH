# MakhOS Phase 19: Signals, process groups & TTY line discipline

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a kernel signal subsystem, process groups/sessions, and a
TTY line discipline; `Ctrl+C` kills the foreground job and spares the
background; 110 in-kernel tests, fuzzed, stress clean
**Depends on:** Phase 12 (scheduler/waits), Phase 18 (console/devices)

---

## 1. Scope

This phase builds the **signal subsystem** and the **terminal line discipline**
that turn a keystroke into a dead process:

```
keystroke -> tty_input() -> ISIG? ^C -> SIGINT to the terminal's
             foreground process group -> default action: terminate
```

Both acceptance tests pass: **`Ctrl+C` kills a process** (through the foreground
process group) and **job control works** (a background group survives `^C`).

**Deferred to Phase 20** (they need the persistent, preemptible user-process
model, which is its own phase): running a **user-installed handler** and
`sigreturn` in ring 3, **async** delivery to a running user process,
`SIGCHLD`/`waitpid` reaping of user processes, and `SIGCONT` stop/continue
scheduling. Also deferred: the **framebuffer console** (PSF font + ANSI) — a
large, cosmetic graphics change; the line discipline here runs over the
existing VGA/serial console, which is what matters for `^C` and job control.

---

## 2. Signals (`signal/signal.c`)

Each thread carries, in its PCB:

- `sig_pending` — bitmask of pending signals;
- `sig_blocked` — the `sigprocmask` mask (`SIGKILL`/`SIGSTOP` can never be blocked);
- `sig_ignore` — per-signal "ignore" disposition;
- `pgid`, `sid` — process group and session.

API: `signal_send(thread, sig)`, `signal_send_pgrp(pgid, sig)`,
`signal_kill(pid, sig)` (with the POSIX `pid`/`0`/`-pgid` semantics),
`signal_procmask`, `signal_set_ignore`, `signal_pending`, and
`signal_take_terminate`.

**Default actions.** The fatal signals (`SIGINT`, `SIGKILL`, `SIGSEGV`,
`SIGTERM`, `SIGQUIT`, …) default to *terminate*; `SIGCHLD`/`SIGCONT` default to
*ignore*. A thread acts on its signals at a safe point by calling
`signal_take_terminate()`, which returns the signal number of a pending,
unblocked, fatal signal; the thread then exits `128 + signo` (the shell
convention). Kernel threads check cooperatively — asynchronous preemptive
delivery belongs to the ring-3 user-process model (Phase 20).

**EINTR.** `signal_send` to a thread blocked in an interruptible wait wakes it
(via the scheduler) and sets a flag; `sched_wait_event` then returns `2`
(EINTR) so the waiter re-checks its signals instead of sleeping on.

## 3. Process groups & sessions

New threads start in their own group and session (`pgid = sid = pid`).
`setpgid`/`getpgid` move a thread between groups; `setsid` starts a new session.
The terminal tracks one **foreground process group**; a signal from a control
character goes to every thread in it.

## 4. TTY line discipline (`tty/tty.c`)

A `termios` with `ICANON`, `ECHO`, `ISIG` and a control-character table drives
`tty_input(byte)`:

- **ISIG**: `^C` → `SIGINT`, `^Z` → `SIGTSTP`, `^\` → `SIGQUIT`, all sent to the
  foreground group;
- **canonical** mode buffers a line, echoes it, handles erase (backspace/DEL),
  and completes the line on `\n`;
- **`^D`** delivers the current line, or EOF (a zero-length read) on an empty one;
- **raw** mode (ICANON off) passes bytes straight through.

`tty_read` returns completed line data; `tty_set_foreground`/`tty_get_foreground`
are the `tcsetpgrp`/`tcgetpgrp` of this phase. Rendering still uses the existing
console.

## 5. Syscall

`kill`(62) is wired to `signal_kill`. The rest of the subsystem is reached
through the kernel API and is exercised directly by the tests (the ring-3
`sigaction`/`sigreturn` path is Phase 20).

## 6. Fuzzed

A KFUZZ **`tty`** target feeds the line discipline arbitrary bytes (it must
never overrun its line buffer) and toggles termios flags and signal masks, with
the foreground group pinned to nobody so control characters harm nothing. The
global heap/tree oracles run after each campaign and the sandbox catches faults.
A 40,000-iteration campaign found no crashes or inconsistencies.

## 7. Tests (`kernel/tests/test_signal.c`)

- **`ctrl_c_kills_foreground_pgroup_only`** — three threads in one group are the
  foreground job, one thread is a background group; `tty_input(^C)` kills the
  three (`exit 128+SIGINT`) and leaves the background thread running.
- **`kill_terminates_a_thread`** — `signal_send(SIGKILL)` → the thread exits.
- **`blocked_signal_stays_pending`** — a masked `SIGINT` is withheld, then
  delivered after `sigprocmask(SIG_UNBLOCK)`.
- **`ignored_signal_is_dropped`** — an ignored disposition discards the signal.
- **`signal_interrupts_blocking_wait_eintr`** — a signal wakes a blocked thread
  with EINTR (return `2`).
- **`process_groups_and_sessions`** — `setpgid`/`getpgid`/`setsid`.
- **`tty.canonical_line_editing`**, **`tty.eof_returns_zero`** — line buffering,
  erase, and `^D` EOF.

**110 in-kernel tests pass**; `make stress` is clean.

## 8. Deferred to Phase 20

| Item | Why it waits |
|---|---|
| ring-3 `sigaction` + handler frame + `sigreturn` | needs a persistent user process to deliver to on return to user mode |
| async delivery to a running user process | the current ring-3 run is non-preemptive |
| `SIGCHLD` / `waitpid` for user processes | needs the user process lifecycle |
| `SIGCONT` stop/continue | needs stopped-state scheduling |
| framebuffer console (PSF + ANSI) | a graphics rewrite of the console; cosmetic to job control |
