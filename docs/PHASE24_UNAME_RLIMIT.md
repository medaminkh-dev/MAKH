# MakhOS Phase 24 (U2): uname and resource-limit syscalls

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `uname`, `getrlimit`/`setrlimit` and `prlimit64` are
implemented, so programs that query the system identity and resource limits at
startup run instead of failing on `-ENOSYS`. **183 in-kernel tests.**
**Depends on:** 20-A (process model)

---

## 1. Scope

The first brick of broadening the syscall surface (the second half of the
"expand userspace" arc). These are the small identity/limit calls a C runtime or
a coreutil touches early:

- **`uname`** — fills a `utsname` (six NUL-padded 65-byte fields): sysname
  `MAKH`, nodename `makh`, release `0.1.0-dev`, version `MAKH 0.1.0-dev x86_64`,
  machine `x86_64`, domainname `(none)`. Makes `/bin/uname` (a busybox applet,
  U1-b) actually work.
- **`getrlimit` / `prlimit64`** — MAKH enforces no limits, so they report
  generous values: `RLIMIT_NOFILE` = the fd-table size (so an `fd_set` sized
  from it is correct), `RLIMIT_STACK` = 8 MiB current / infinite max, everything
  else infinite. `prlimit64` additionally fills the caller's "old" buffer.
- **`setrlimit`** — accepted as a no-op (nothing to enforce).

`sysinfo` is deliberately left out of this brick (its struct layout is fiddly
and no current program needs it); it can follow when something does.

## 2. Test

- **`syscalls.uname_and_getrlimit_from_userspace`** — `/bin/unametest`
  (`user/unametest.c`) calls `uname(2)` and `getrlimit(2)` through the raw
  syscall path and exits 42 iff `uname` reports sysname `MAKH` and an x86
  machine string, and `getrlimit(RLIMIT_NOFILE)` returns a nonzero current
  limit. Distinct non-42 codes mark which check failed.

**183 in-kernel tests pass**; `make stress` (24×4) is clean.

## 3. Next

The larger syscall-breadth item is `poll`/`select` (and `epoll`), which need a
pollable-fd abstraction over pipes, the TTY and sockets — the groundwork for
richer programs and, later, user-facing networking.
