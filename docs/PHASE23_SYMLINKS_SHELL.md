# MakhOS Phase 23 (U1): symlinks, and a shell with commands

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the VFS supports symbolic links, and busybox's applets
are reachable as `/bin/<name>` so MAKH has a real set of commands. **182 in-kernel
tests.**
**Depends on:** 18 (VFS/tmpfs/initrd), 20-O2 (busybox)

---

## 1. Scope

Two bricks that make MAKH feel like a system you can actually use at the shell:

- **U1-a — symbolic links** in the VFS: a new `VNODE_LNK` type, path resolution
  that follows links, `readlink`/`symlink` syscalls, and the initrd loader
  honouring tar symlink entries.
- **U1-b — commands**: busybox's applets exposed as `/bin/<name>` symlinks
  (`ls`, `cat`, `grep`, `rm`, `cp`, `mv`, `mkdir`, `pwd`, `head`, `tail`, `wc`,
  `true`, `false`, `ash`, …), so a user runs `ls`, not `busybox ls`.

## 2. Symlinks (U1-a)

- **Type/storage**: `VNODE_LNK`; tmpfs keeps the target string in the same buffer
  a regular file uses. `tmpfs` gains `symlink`/`readlink` ops.
- **Resolution**: `resolve_from()` (the rewritten path walker) follows a symlink
  whenever it meets one — always for an interior component, and for the final
  component unless the caller asked not to (`vfs_resolve_nofollow`, for
  readlink/lstat). A target is re-resolved from the root if absolute, else from
  the directory holding the link. A cycle is bounded by `VFS_SYMLINK_MAX` (8)
  and returns `NULL` (ELOOP) instead of hanging.
- **initrd**: the tar loader handles typeflag `'2'` (symlink), creating the link
  from the header's `linkname`.
- **syscalls**: `symlink`/`symlinkat` (store a link; the target is kept verbatim,
  need not exist) and `readlink`/`readlinkat` (copy the target out, POSIX-style
  without a NUL).

## 3. Commands (U1-b)

The `$(INITRD)` recipe creates `/bin/<applet>` as a **relative** symlink to
`busybox`. When the kernel execs e.g. `/bin/ls`, resolution follows the link to
`/bin/busybox` and loads it, while `argv[0]` stays `ls` — busybox's multi-call
dispatch then runs the `ls` applet. No change to `execve` was needed: it already
resolves the path (now through the symlink) and passes the caller's argv.

MAKH keeps its own `/bin/sh` and `/bin/echo` (small programs its tests drive);
busybox's shell is therefore `/bin/ash`. `ln -s` (no `-f`) is used so the build
fails loudly if an applet name ever collides with a real program in `/bin`.

## 4. Tests

- **`vfs.symlink_follow_readlink_and_loop`** — a link resolves to its target
  file; `readlink` returns the target; a link to a directory is walked through
  to a child; a 2-link cycle resolves to NULL.
- **`busybox.applets_via_symlink`** — `/bin/true` → 0, `/bin/false` → 1 (applet
  dispatch via `argv[0]` through the symlink), and `/bin/ash -c 'exit $((5+2))'`
  → 7.

**182 in-kernel tests pass**; `make stress` (24×4) is clean.

## 5. Next

More syscalls (poll/select/epoll, getrlimit, sysinfo, …) so still more real
programs run unchanged; optionally promoting busybox `ash` to `/bin/sh`.
