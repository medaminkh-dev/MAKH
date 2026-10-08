# MakhOS Phase 20-R (G2-c): the ext2 write path

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — MAKH creates files on an on-disk ext2 filesystem and
writes to them; the bytes read back (from a fresh inode read) match. **166
in-kernel tests.**
**Depends on:** 20-Q (ext2 read)

---

## 1. Scope

G2-b read an ext2 disk; this brick writes one. It closes the storage gap enough
for F21: a program (or the kernel) can create a file and fill it with bytes that
survive to a re-open. Writing is what lets a self-hosting compiler emit object
files to disk.

- **Create** regular files and **write** to them (append and in-place
  overwrite), allocating data blocks and inodes as needed, through direct and
  single-indirect blocks.
- **Scoped** to block group 0 (the test image is one group), direct +
  single-indirect, and regular files. **Deferred (G2-d):** `mkdir`, `unlink`,
  `truncate`, double/triple-indirect, and multi-group allocation.

## 2. Allocation (`fs/ext2.c`)

- **`alloc_block` / `alloc_inode`** flip the first free bit in group 0's block
  / inode bitmap and write the bitmap straight back, so the same block or inode
  is never handed out twice. A freshly allocated data block is zeroed on disk.
- **`dec_free_counts`** then keeps the two free-count mirrors — the group
  descriptor and the superblock — honest, so the filesystem stays
  self-consistent across allocations.
- **`write_inode`** read-modify-writes the inode's 128-byte slot in the inode
  table, so neighbouring inodes are preserved; a newly created inode is written
  with a **zeroed block map**, so a reused inode never resurrects stale block
  pointers (the subtle correctness trap in ext2 reuse).

## 3. Writing (`fs/ext2.c`)

- **`alloc_file_block`** maps a file-relative block to a freshly allocated disk
  block, wiring it into the inode's direct slots or, past block 12, into a
  (lazily allocated) single-indirect block.
- **`ext2_write`** walks the byte range block by block: a partial block is
  read-modify-written, a full block is written whole; a hole is filled by
  allocating. It grows `i_size` and writes the inode back at the end.
- **`ext2_create`** allocates an inode, writes a clean regular-file inode
  (mode `0644`, one link, empty map), then adds a directory entry.
- **`dir_add_entry`** inserts `{inode, type, name}` by splitting the slack in an
  existing entry's `rec_len`, or — if no entry has room — by growing the
  directory with one more block.

## 4. Fuzzed

Still no ext2 KFUZZ target (the natural one feeds a **corrupt image** to the
read/mount path and belongs with hardening). The write path is exercised by the
create/write/overwrite KTESTs; every allocation is bounded by the bitmap size
and every write by the byte count, so nothing loops on bad state.

## 5. Tests (`kernel/tests/test_ext2.c`)

Each write test only touches a file it creates, so the read-only fixtures stay
intact (writes are ephemeral under QEMU `snapshot=on`, but live for the boot):

- **`ext2.create_write_readback`** — create `/mnt/created.txt`, write 15000
  bytes (forcing block allocation through the single-indirect map), re-resolve,
  and read the exact bytes back.
- **`ext2.overwrite_in_place`** — create a file, write ten bytes, overwrite two
  in the middle, and read back the spliced result (partial-block RMW).

**166 in-kernel tests pass**; `make stress` (serial) is clean.

## 6. Deferred to G2-d

`mkdir` (needs `.`/`..` and parent link-count bookkeeping), `unlink`/`rmdir`
(free blocks + inode on the last link), `truncate`, and allocation beyond block
group 0 / past the single-indirect level. With create+write in hand, F21's
toolchain can already emit files; the rest hardens it into a general filesystem.
