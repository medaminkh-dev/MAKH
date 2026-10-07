# MakhOS Phase 20-F: job control (`setpgid` + `tcsetpgrp`)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — each shell command runs in its own process group and owns
the terminal while it runs, so Ctrl+C kills the *job*, not the shell. 144
in-kernel tests
**Depends on:** Phase 19 (process groups, `signal_send_pgrp`, line discipline),
Phase 20-D (controlling terminal, the shell), Phase 20-A-2 (`fork`/`execve`)

---

## 1. Scope

Phase 20-D wired `^C` to the terminal's foreground process group, but *the shell
itself was that group*, so Ctrl+C killed the shell. Job control fixes this: the
shell gives each command its own process group and hands the terminal to that
group for the duration of the job, reclaiming it when the job exits.

New kernel surface:

- **`setpgid` / `getpgid` / `getpgrp`** — move a process (self or a child) into
  a process group, and read a process's group back;
- **`setsid`** — become a new session + group leader (for daemons; the shell
  doesn't need it, but the API is here);
- **`ioctl(TIOCSPGRP/TIOCGPGRP)`** — the mechanism behind `tcsetpgrp` /
  `tcgetpgrp`: set or read the controlling terminal's foreground group.

And the shell now drives them to implement real foreground job control.

**Deferred:** user-installed signal handlers (`sigaction`/`sigprocmask`/
`sigreturn`) — the shell cannot yet *block* SIGINT across the fork/`setpgid`
window, so a Ctrl+C in that sub-millisecond gap could still reach the shell;
closing that window is the very next brick. Also deferred: background jobs
(`&`, `SIGTTIN`/`SIGTTOU` for background read/write), `SIGTSTP` stop/`SIGCONT`
continue, and `waitpid(WUNTRACED)`.

---

## 2. Process-group syscalls (`signal.c`, dispatched from `syscall.c`)

Processes already carried `pgid`/`sid` (Phase 19), and every new process starts
as its own group and session leader (`pgid == sid == pid`, set in
`proc/core`). Phase 19 also already had `sys_setpgid`/`sys_getpgid`/`sys_setsid`
in `signal.c`; rather than duplicate them, the ring-3 dispatch routes straight
to those (one implementation), and `sys_setpgid` gained the permission checks a
user-facing call needs. The syscalls expose and mutate the group state:

- **`setpgid(pid, pgid)`** — `pid == 0` means the caller; `pgid == 0` means name
  the group after the target's own pid (lead a new group). A process may only
  move itself or one of its children, and only within its own session
  (`-ESRCH`/`-EPERM` otherwise). This is exactly the operation a shell performs
  on each child.
- **`getpgid(pid)` / `getpgrp()`** — read a process's group (`getpgrp` is the
  caller's).
- **`setsid()`** — set `sid = pgid = pid`. (POSIX forbids a group leader from
  calling `setsid`; our processes are always leaders at birth, so we relax that
  rule — harmless here, and real daemons will want it.)

## 3. Terminal ownership via `ioctl` (`syscall.c`, `tty.c`)

`tcsetpgrp`/`tcgetpgrp` are thin wrappers over `ioctl`:

- **`ioctl(fd, TIOCSPGRP, &pgid)`** sets the terminal's foreground group
  (`tty_set_foreground`), and **`TIOCGPGRP`** reads it back. `fd` must be the
  controlling terminal (0/1/2) or the call is `-ENOTTY`; an unknown request is
  `-EINVAL`. The `pgid` argument is copied through `copy_from_user`/
  `copy_to_user` like any other user pointer.

This is the first `ioctl` in the kernel; the surface is deliberately tiny (only
the two job-control requests) and grows as devices need it.

## 4. The shell does job control (`user/sh.c`)

The run-a-command loop now reads:

```
shell_pgid = getpgrp();                 // the shell leads its own group
...
pid = fork();
if (pid == 0) {                         // child
    setpgid(0, 0);                      // lead a new group (race-safe)
    execve(argv[0], argv, 0);
}
setpgid(pid, pid);                      // parent sets it too (race-safe)
tcsetpgrp(0, pid);                      // the job owns the terminal
waitpid(pid, &last);
tcsetpgrp(0, shell_pgid);               // the shell reclaims it
```

Both parent and child call `setpgid` to the same value, so the group is
established no matter which runs first. While the job holds the terminal, `^C`
raises SIGINT on *its* group; the shell is in a different group and is spared.

## 5. Fuzzed

No new KFUZZ target: the new syscalls are small, pure process-table and
single-variable (`fg_pgid`) mutations with no parsing of untrusted buffers, and
their error paths (`-ESRCH`, `-EPERM`, `-ENOTTY`, `-EINVAL`, `-EFAULT`) are
exercised directly by `/bin/jobtest`. The signal-delivery path that makes Ctrl+C
work was already hammered by the Phase 19 `signal` target.

## 6. Tests (`kernel/tests/test_job.c`) + programs (`user/`)

- **`job.syscalls_roundtrip`** (`/bin/jobtest`) — from ring 3: a fresh process
  leads its own group (`getpgrp() == getpid()`), `setpgid(0,0)` keeps it there,
  `tcsetpgrp`/`tcgetpgrp` round-trips the foreground group, and `ioctl` on a
  non-terminal fd is `-ENOTTY`. Exits 0 iff every step behaved.
- **`job.ctrl_c_kills_job_not_shell`** (`/bin/sh`, `/bin/spinner`, `/bin/hello`)
  — the shell runs `spinner` (never returns on its own) as a foreground job;
  `^C` kills it. We prove the shell *survived* by having it run `hello`
  (exits 42) afterwards and reading that status back — had `^C` killed the shell
  instead, `waitpid` would see 130, not 42.

**144 in-kernel tests pass**; `make stress` (serial) is clean.

## 7. Deferred to the next brick (signals)

| Item | Why it waits |
|---|---|
| block SIGINT across fork/`setpgid` | needs `sigprocmask` from ring 3 |
| user-installed handlers (`sigaction`/`sigreturn`) | needs a signal frame on the user stack |
| background jobs (`&`, `SIGTTIN`/`SIGTTOU`) | needs read/write to check the fg group |
| stop/continue (`SIGTSTP`/`SIGCONT`, `WUNTRACED`) | needs a stopped process state |

Job control closes the second item of gap #1. The last piece is **user signal
handling** — `sigaction`/`sigprocmask`/`sigreturn` with a signal frame — which
also lets the shell close the tiny Ctrl+C race noted above.
