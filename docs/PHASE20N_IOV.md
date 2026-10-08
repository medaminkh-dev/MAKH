# MakhOS Phase F20-a: musl-readiness syscalls (`writev`/`readv`/`exit_group`)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — scatter/gather I/O and `exit_group` are in, so a C
library's buffered stdio and exit path work. 157 in-kernel tests
**Depends on:** Phase 20-K (the fd layer / pipes), Phase 20-I (the syscall path)

---

## 1. Scope

The first step of **F20 (the musl port)**: close the small syscall gaps a static
libc touches the moment it starts, before fetching and building musl itself.

- **`writev` / `readv`** — scatter/gather I/O. musl's buffered stdio writes
  through `writev` (a header iov + the buffer), so `printf`/`puts` need it;
- **`exit_group`** — musl's `_Exit`/`exit` use `exit_group(231)`, not the bare
  `exit(60)`. With no thread groups yet it is identical to `exit`;
- **`madvise`** — the mallocng allocator calls it (`MADV_DONTNEED`/`FREE`); an
  advisory no-op (`return 0`) is a valid implementation.

Everything else musl needs at startup already exists: `arch_prctl(SET_FS)`
(20-H), `set_tid_address`/`futex` (20-L), `brk`/`mmap` (20-B), `rt_sigprocmask`/
`rt_sigaction` (20-G), `ioctl` (20-F — an unknown request returns `-EINVAL`, so
musl's tty probe decides stdout is fully buffered and flushes via `writev`).

**Deferred to the build step (F20-b):** actually compiling musl against this
ABI, running a standard C program, and booting `busybox sh`.

---

## 2. `writev` / `readv` (`syscall.c`)

Both walk the user `iovec` array and reuse the existing `do_write`/`do_read`
per entry, so pipe/console/file routing and `copy_to/from_user` validation are
inherited unchanged. The count is bounded (`> 1024 → -EINVAL`), a zero-length
entry is skipped, and a short transfer on any entry stops the walk and returns
the bytes moved so far — standard `writev`/`readv` semantics.

## 3. `exit_group` (`syscall.c`)

Routed to the same handler as `exit`: the process becomes a zombie with
`code & 0xff`. When real `CLONE_THREAD` groups arrive, this is where
"terminate every thread in the group" will hook in; today one process is one
thread, so the two are the same.

## 4. Fuzzed

No new KFUZZ target: `writev`/`readv` are thin loops over the already-fuzzed
read/write paths, with a bounded count and per-entry `copy_from_user` of the
iovec (a bad array returns `-EFAULT`). `exit_group`/`madvise` are a relabel and
a no-op.

## 5. Tests (`kernel/tests/test_iov.c`) + program (`user/iovtest.c`)

- **`iov.writev_readv_over_a_pipe`** (`/bin/iovtest`) — `writev` two chunks
  (`"ab"`, `"cde"`) into a pipe, `readv` them back into two separate buffers,
  and verify the bytes reassembled (`ab` + `cde`). Exit **44**.

**157 in-kernel tests pass**; `make stress` (serial) is clean.

## 6. Next

F20-b: fetch the musl source, build it static against this syscall ABI (the
Linux numbers we already use), link a standard C program with it and run it on
MAKH, then `busybox sh`.
