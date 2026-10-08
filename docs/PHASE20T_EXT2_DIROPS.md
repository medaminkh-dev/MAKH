# MakhOS Phase 20-T (G2-d): ext2 directory ops — mkdir / unlink / rename / truncate

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the ext2 filesystem is now read/write in full for the
common cases: create directories, remove files and empty directories, rename
(with replace), and truncate. **171 in-kernel tests.**
**Depends on:** 20-R (ext2 write)

---

## 1. Scope

G2-c could create and write files; this brick finishes the everyday filesystem:

- **`mkdir`** — a new directory with its `.`/`..` entries and the parent's link
  count bumped;
- **`unlink`** — remove a file (free its blocks + inode on the last link) and,
  for an empty directory, act as **`rmdir`**;
- **`rename`** — re-home a name within the mount, replacing an existing target,
  with a moved directory's `..` and both parents' link counts fixed up on a
  cross-directory move;
- **`truncate`** — free the blocks past a new length (and the single-indirect
  block when nothing past the direct blocks survives).

Still deferred: double/triple-indirect and allocation beyond block group 0 (the
test image is one group); hard links.

## 2. Freeing (`fs/ext2.c`)

The mirror of G2-c's allocation: `free_block`/`free_inode_num` clear the group-0
bitmap bit and bump the group + superblock free counts (`inc_free_counts`), and
`free_from(vi, n)` releases every file block from index `n` up — including the
single-indirect block itself once only direct blocks remain. `truncate` is a
thin wrapper: free from `ceil(len/bs)`, set the size, write the inode.

## 3. Directory surgery (`fs/ext2.c`)

- **`dir_remove_entry`** unlinks a name from a directory block by merging its
  record into the previous entry (or voiding the first slot), returning the
  inode but *not* touching its link count — so both `unlink` (which then frees)
  and `rename` (which re-homes) build on it.
- **`ext2_create`** now also makes directories: it allocates a data block,
  writes `.` (self) and `..` (parent), sets `links=2`, adds the parent entry and
  bumps the parent's link count.
- **`ext2_unlink`** frees a file on its last link; for a directory it checks
  emptiness (`dir_is_empty`) and, if empty, frees it and drops the parent's
  link — i.e. it doubles as `rmdir`.
- **`ext2_rename`** looks the source up, unlinks any existing target, adds the
  new name, removes the old one, and for a cross-directory directory move
  rewrites `..` and adjusts both parents' link counts.

## 4. VFS + syscalls

`vfs_ops` gained a `rename(olddir, oldname, newdir, newname)` hook;
`vfs_rename` resolves both parents and requires the *same filesystem* (same ops
table). The syscall layer wires `mkdir`/`mkdirat`, `rmdir`, `unlink`/`unlinkat`
(G3) and `rename`/`renameat`/`renameat2` onto `vfs_mkdir`/`vfs_unlink`/
`vfs_rename`, serving `AT_FDCWD` and absolute paths.

## 5. Fuzzed

No new KFUZZ target; the directory-surgery paths are bounded (entry `rec_len`
walks stop at the block end, allocation at the bitmap size) and are covered by
the KTESTs below. A corrupt-image ext2 fuzz target remains the right follow-up
for hardening the whole driver at once.

## 6. Tests (`kernel/tests/test_ext2.c`)

- **`ext2.mkdir_create_unlink_rmdir`** — `mkdir /mnt/d1`, create and read a file
  inside it, unlink the file, then rmdir the now-empty directory.
- **`ext2.rename_replaces`** — rename over an existing target; the source name
  disappears and the destination holds the moved bytes.
- **`ext2.truncate_to_zero`** — write 5000 bytes, truncate to 0, and see the
  size reset (blocks freed) from a fresh resolve.

**171 in-kernel tests pass**; `make stress` (serial) is clean.

## 7. Next

Storage (gap #2) is now read/write-complete for the common cases. The road ahead
is **F21** — a self-hosting toolchain (port a C compiler, assemble + link, run
`make`, rebuild MAKH on itself), which these filesystem and syscall pieces exist
to support.
