# MakhOS Phase 30: chmod — real, mutable file permissions

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — files have a real, mutable permission mode: `chmod` changes
it, `ls -l` shows it, and on ext2 it persists to the inode. **184 in-kernel
tests.**
**Depends on:** 18 (VFS + tmpfs), 20-J (`stat`), 20-Q/R (ext2 read/write)

---

MAKH's own implementation — no code is lifted from Linux. The permission model
is a plain POSIX-compatible mode (so `ls -l` and musl agree), but every line is
original and the design is deliberately minimal.

## 1. A mode on the vnode

Before this brick the mode was *synthesised* from the vnode type in
`fill_stat()` (every regular file was `0644`, every directory `0755`), so there
was nothing to change. Now `vnode_t` carries a `uint32_t mode` (the permission
bits; the type still lives in `vnode_t.type`). `fill_stat()` reports
`type_bits | (vn->mode & 07777)`, falling back to the per-type default when a
vnode's mode is still unset (0).

## 2. The VFS chmod path

- `vfs_chmod(path, mode)` resolves the path (following symlinks, as POSIX
  `chmod` does), stores `mode & 07777` on the vnode, and — if the filesystem
  provides one — calls the new `.chmod(vnode, mode)` op.
- **tmpfs** needs no op: the vnode lives in memory, so the stored mode is the
  truth and survives for the life of the file.
- **ext2** implements `.chmod`: it rewrites the inode's `i_mode` (keeping the
  type bits) and calls `write_inode()`, so the change is persisted to disk.
  `make_vnode()` now also seeds `vnode.mode` from the on-disk `i_mode`, so
  `ls -l` shows a real ext2 file's actual permissions.

## 3. Syscalls

`chmod(2)` (90) and `fchmodat(2)` (268) are wired to `do_chmod`, which copies the
path in and calls `vfs_chmod`. `fchmodat`'s dirfd/flags are ignored (paths are
cwd-relative or absolute, as elsewhere in MAKH). The mode argument is the usual
octal permission word.

## 4. The command

`user/chmod.c` — `chmod MODE FILE...` — parses an octal mode (3–4 digits,
rejecting non-octal input) and applies it to each file with the new `uchmod`
wrapper. `ls -l` renders the result (`chmod 600 f` → `-rw-------`,
`chmod 755 f` → `-rwxr-xr-x`). Added to the `help` guide.

## 5. Verification

- **184 in-kernel tests pass** (new `vfs.chmod_changes_mode`: default `0644` →
  `chmod 0600` → stat shows `0600` with the type preserved; missing path →
  `-ENOENT`); `make stress` (24×4) clean; license headers ok.
- **Interactive (QMP send-key):** `touch /tmp/cf` then `ls -l` → `-rw-r--r--`;
  `chmod 600` → `-rw-------`; `chmod 755` → `-rwxr-xr-x`. Adversarial:
  `chmod 999` → "invalid mode"; `chmod 644 /nope` → "cannot change mode";
  `chmod` with no args → usage.

## 6. Not in this brick (next)

- Symbolic modes (`u+x`, `go-w`) — only octal is accepted today.
- `fchmod(2)` by descriptor (only path-based chmod/fchmodat are wired).
- Ownership (`chown`) and a real uid/gid model — MAKH is single-user (root).
