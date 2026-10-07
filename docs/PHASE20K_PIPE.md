# MakhOS Phase 20-K: pipes, `dup2`, and shell pipelines

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `pipe`/`pipe2`/`dup2` work, fds are inherited across
`fork` and released on exit, and the shell runs `a | b` pipelines. 153 in-kernel
tests
**Depends on:** Phase 18 (VFS, fd table), Phase 20-A-2 (fork), Phase 20-F (the
shell), Phase 19 (SIGPIPE)

---

## 1. Scope

Brick 1.3 of Stage 1, and the one that reshapes the descriptor layer. Until now
an fd referred to at most one thing, fds 0/1/2 were a hardcoded console
fast-path, and `fork` did **not** carry open files. That is exactly what pipes
and redirection need to change.

- **`pipe`/`pipe2`** — a kernel pipe (a blocking, bounded ring buffer) with a
  read end and a write end;
- **`dup2`** — point a second fd at an existing open description (so stdin/stdout
  can be redirected onto a pipe);
- **fd inheritance** — `fork` now shares the parent's open descriptions with the
  child (refcounted), and **exit** releases them (so a pipe peer sees EOF/EPIPE);
- **shell pipelines** — `sh` splits a line on `|` and wires each stage's stdout
  to the next stage's stdin.

**Deferred:** per-pipeline process groups (a pipeline shares the shell's group
for now, so Ctrl+C during a multi-stage pipeline isn't scoped to it), named
pipes (FIFOs in the filesystem), `O_NONBLOCK` pipe semantics, and `F_SETFD`
`FD_CLOEXEC` (exec keeps fds open).

---

## 2. Open descriptions become shareable (`fs/vfs.c`)

The fix underneath everything: an `open file description` (`file_t`) now carries
a **refcount** = how many fds point at it, and a vnode's refcount = how many
`file_t`s reference it. One helper, `file_put`, releases a reference: at the last
fd it detaches a pipe end, drops the vnode's open-count, and frees a pipe's
vnode when no end remains. `dup2` and `fork` just bump the `file_t` refcount;
`close` and `exit` call `file_put`. This replaces the old "one fd owns its
`file_t`, freed on close" model without changing regular-file behaviour.

## 3. The pipe (`fs/vfs.c`)

A pipe is a `VNODE_FIFO` vnode whose `priv` is a 4 KiB ring buffer plus reader
and writer counts and two wait queues:

- **read** returns available bytes immediately; on an empty pipe it blocks on
  the read queue, unless there are no writers left — then it's EOF (0). A signal
  returns `-EINTR`.
- **write** copies into the ring, waking readers; a full pipe blocks on the
  write queue; **no readers** raises `SIGPIPE` and returns `-EPIPE`.

Because a pipe is just a vnode with `read`/`write` ops, it rides the existing
`vfs_fd_read`/`vfs_fd_write` path — no special case in the fd layer. The
reader/writer counts track the *descriptions* (one read-end `file_t`, one
write-end `file_t`), so they fall to zero only when every fd (across dups and
forks) referring to that end is closed — which is the moment the peer must see
EOF or EPIPE.

## 4. fds 0/1/2: table-first, console-fallback (`syscall.c`)

The one change to the hot path: `read`/`write` now consult the fd table *first*.
If a descriptor exists for the fd (a file, or a pipe `dup2`'d onto 0/1/2), it is
used; only when the slot is **empty** do 1/2 fall back to the console and 0 to
the tty. So the default stdio path is byte-for-byte unchanged — the regression
guard for this refactor — while a pipeline can redirect stdin/stdout by simply
populating the slot.

## 5. `fork` inherits, `exit` releases (`proc/user.c`, `sched.c`)

`fork` now duplicates the fd table into the child, sharing each description
(refcounts bumped) — POSIX semantics, and what lets a forked stage keep the
pipe ends it needs. `thread_exit` calls `vfs_close_all` at **zombie** time (not
at reap), so the instant a pipeline stage exits, its pipe ends close and the
peer unblocks.

## 6. The shell runs pipelines (`user/sh.c`)

A line with `|` is split into stages; the shell creates a pipe between each
adjacent pair, forks a child per stage that `dup2`s its ends onto 0/1 and execs,
closes the ends it no longer needs, and waits for all of them (the last stage's
status is the result). A single command keeps the full job-control path from
20-F unchanged, so Ctrl+C still targets a lone foreground job.

## 7. Fuzzed

No new KFUZZ target: a pipe's read/write **block**, which a single-threaded
sandbox can't drive without a partner to drain it, and the surface is a bounded
ring buffer with refcounts rather than a parser of untrusted bytes. The
adversarial paths — EOF with no writers, `-EPIPE`/`SIGPIPE` with no readers,
`-EINTR` on a signal, a short read, `dup2` onto a live fd (closing it first),
and fd release across fork/exit — are exercised directly by the acceptance
program and the shell pipeline, and `file_put` is the single chokepoint for
the refcount bookkeeping.

## 8. Tests (`kernel/tests/test_pipe.c`) + programs (`user/`)

- **`pipe.pipe_dup2_fork_mechanism`** (`/bin/pipetest`) — a child writes down a
  pipe and the parent reads it then sees EOF once the child closes and exits;
  then a child `dup2`s the pipe onto stdout and `write(1,…)` reaches it. Exits
  **55**.
- **`pipe.shell_runs_a_pipeline`** (`/bin/sh`, `/bin/echo`, `/bin/countin`) —
  the shell runs `echo abc | countin`; the 4 bytes `"abc\n"` travel through the
  pipe and `countin` exits with the count, so the shell reports **4**.

**153 in-kernel tests pass**; `make stress` (serial) is clean.

## 9. Deferred

| Item | Why it waits |
|---|---|
| per-pipeline process group | job control for a whole pipeline, not just a lone command |
| named pipes (filesystem FIFOs) | needs a mknod + a FIFO path in the FS |
| `O_NONBLOCK` pipes, `FD_CLOEXEC` | no non-blocking or exec-close semantics yet |

Next in Stage 1: **`futex` + `set_tid_address` + `clone`** (1.4) — the
user-space threading primitives musl's pthreads stand on.
