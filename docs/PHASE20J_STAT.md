# MakhOS Phase 20-J: file metadata & listing (`stat`/`getdents64`/`fcntl`)

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — ring 3 can stat files, list directories, and query/set
descriptor flags: `stat`/`lstat`/`fstat`, `getdents64`, `fcntl`. 152 in-kernel
tests
**Depends on:** Phase 18 (VFS, vnodes, `readdir`), Phase 20-A (user processes)

---

## 1. Scope

Brick 1.2 of Stage 1. A program could open/read/write files but couldn't ask
*what* a path is or *what's in* a directory — so `ls`, `find`, shell globbing,
and a libc's `opendir`/`stat` were all impossible. This adds the metadata and
listing surface.

- **`stat(path)` / `lstat(path)` / `fstat(fd)`** — fill a `struct stat` (type,
  size, …). No symlinks yet, so `lstat` aliases `stat`.
- **`getdents64(fd, buf, n)`** — read directory entries as Linux
  `linux_dirent64` records; call again to continue (the fd offset is the
  cursor).
- **`fcntl(fd, cmd, arg)`** — `F_GETFL`/`F_SETFL` (the `O_APPEND`/`O_NONBLOCK`
  bits), `F_GETFD`/`F_SETFD` accepted. `F_DUPFD` waits for 1.3 (it needs shared
  open-file descriptions, which arrive with `dup2`/`pipe`).

**Deferred:** symlinks (`lstat` is a true alias for now), real timestamps in
`struct stat` (zero until the RTC, 1.5), `statx`, and `FD_CLOEXEC` tracking.

---

## 2. Byte-exact `struct stat` (`fs/vfs.h`)

The struct is laid out **byte-for-byte as Linux x86-64** — `st_dev`, `st_ino`,
`st_nlink` (8 bytes each), then the 4-byte `st_mode`/`st_uid`/`st_gid`/`__pad0`,
`st_rdev`, `st_size`, block counts, and three embedded timespecs — 144 bytes
total. That means when musl is ported (F20) its own `struct stat` will find
every field at the offset it expects, with no translation shim. `fill_stat`
maps a vnode's type to the mode bits (`S_IFREG`/`S_IFDIR`/`S_IFCHR` + perms),
reports `vnode->size`, and uses the vnode pointer as a stable `st_ino`. `fstat`
on fds 0/1/2 (which have no `file_t`) returns a synthetic character-device stat
— the controlling terminal.

## 3. `getdents64` over the VFS `readdir` op (`fs/vfs.c`)

`vfs_getdents` walks the directory's existing `readdir(dir, index, name)` op,
using the open file's offset as the entry cursor so repeated calls resume where
they left off. For each name it packs a `linux_dirent64` — `d_ino`, `d_off`,
an 8-byte-aligned `d_reclen`, a `d_type` (looked up via the dir's `lookup` op:
`DT_DIR`/`DT_REG`/`DT_CHR`), and the NUL-terminated name — stopping when the
buffer can't hold the next record (and returning `-EINVAL` if it can't hold even
one). The syscall layer fills a bounded kernel buffer and copies it out, so a
hostile or short user buffer never drives kernel writes.

## 4. `fcntl` (`fs/vfs.c`)

Thin and honest: `F_GETFL` returns the open flags, `F_SETFL` updates the
settable bits (`O_APPEND`/`O_NONBLOCK`), `F_GETFD`/`F_SETFD` are accepted as
no-ops (no `FD_CLOEXEC` state yet), and `F_DUPFD` is refused until descriptor
duplication lands in 1.3.

## 5. Fuzzed

No new KFUZZ target: `stat`/`fstat` are fixed-size `copy_to_user`s, `fcntl` is a
small switch, and `getdents64` packs into a bounded kernel buffer with explicit
"does the next record fit" checks. The adversarial cases — a missing path
(`-ENOENT`), a non-directory fd (`-ENOTDIR`), a bad user pointer (`-EFAULT`), and
a buffer too small for one entry (`-EINVAL`) — are on the handlers' own paths and
in the acceptance program; the directory iteration rests on the Phase-18 VFS,
already fuzzed by the `vfs` target.

## 6. Tests (`kernel/tests/test_stat.c`) + program (`user/statls.c`)

- **`stat.stat_fstat_getdents_fcntl_from_userspace`** (`/bin/statls`) — from
  ring 3: `/bin/hello` stats as a non-empty regular file, `/bin` as a directory,
  `fstat(1)` as a character device; listing `/bin` with `getdents64` finds the
  `hello` entry; `fcntl(F_GETFL)` succeeds — exits **88**.
- **`stat.vfs_stat_reports_regular_file`** — a direct kernel check:
  `vfs_stat` distinguishes a regular file from a directory and returns `-ENOENT`
  for a missing path.

**152 in-kernel tests pass**; `make stress` (serial) is clean.

## 7. Deferred

| Item | Why it waits |
|---|---|
| symlinks + a real `lstat` | no symlink vnode type yet |
| real timestamps in `struct stat` | needs the RTC epoch — brick 1.5 |
| `F_DUPFD` / `FD_CLOEXEC` | needs shared open-file descriptions — brick 1.3 |
| `statx` | not needed until a tool asks for it |

Next in Stage 1: **`pipe` + `dup2`** (1.3) — the fd-model refactor that makes
fds 0/1/2 real table entries and lets the shell wire pipelines.
