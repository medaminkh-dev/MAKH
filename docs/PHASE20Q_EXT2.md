# MakhOS Phase 20-Q (G2-b): a read-only ext2 filesystem

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — MAKH mounts a real on-disk **ext2** filesystem from a
virtio-blk disk into the VFS and reads files from it through the ordinary
`open`/`read` path. **164 in-kernel tests.**
**Depends on:** 20-P (virtio-blk), Phase 18 (VFS)

---

## 1. Scope

G2-a gave MAKH a disk; this brick puts a real filesystem on it. A **read-only
ext2** driver parses an on-disk image built by `mke2fs` and exposes it through
the VFS, so a program opens `/mnt/hello.txt` exactly as it opens a tmpfs file.
This is what lets F21's compiler read sources from a disk that survives a reboot
(writing them back is the next step).

- **Read path only:** superblock + block-group descriptors, inodes, the block
  map (direct + single-indirect), directory-entry walk, `lookup`/`read`/
  `readdir`. **Deferred:** write/create/unlink, double/triple-indirect, and a
  block cache.

## 2. Multiple disks (`drivers/virtio_blk.c`)

The driver now drives up to four disks: `virtio_blk_init` enumerates every
virtio-blk PCI function into a device array, and the read/write API takes a unit
number. The ext2 disk is found by **content, not position** — `ext2_mount_any`
scans the units and mounts the first whose superblock carries the ext2 magic, so
the raw test disk (unit 0) and the ext2 disk coexist without hard-coding order.

## 3. The ext2 driver (`fs/ext2.c`)

- **Mount:** read the superblock at byte 1024, check magic `0xEF53`, record the
  block size, inode size, inodes-per-group and the group-descriptor-table block,
  then build the root vnode (inode 2).
- **Inodes:** `(ino-1)` splits into a group and an index; the group's descriptor
  gives the inode-table block, from which the inode's `i_block[]` map, size and
  mode are read. The vnode's type comes from `i_mode` (authoritative), not the
  directory entry's filetype hint.
- **Block map:** `map_block` resolves a file-relative block to a disk block —
  direct for the first 12, then one level of indirection through `i_block[12]`.
- **Directories:** `dir_walk` streams the entry list in the directory's data
  blocks; `lookup` matches a name, `readdir` returns the n-th entry.

All device I/O goes through a PMM scratch page (the identity-mapped DMA buffer
virtio-blk needs); file bytes are then copied into the caller's buffer, so
callers pass any kernel pointer. Each `lookup` allocates a fresh vnode (an
inode→vnode cache is deferred — fine for the read workload here).

## 4. Fuzzed

No new KFUZZ target yet. The driver reads an on-disk image produced by `mke2fs`,
not adversarial input; a fuzz target that feeds a **corrupt ext2 image** to the
mount/lookup path (the natural analogue of the `elf` target) is a good follow-up
once the write path lands. The read path is bounded — a bad block pointer reads
zeros or returns `-EIO`, it never loops.

## 5. The test image (harness)

`tools/mkext2.sh` builds a bare ext2 image (1 KiB blocks, 128-byte inodes, no
htree/resize features) with `mke2fs -d`: `hello.txt`, `dir/nested.txt`, and a
20000-byte `big.txt` whose tail lives past the 12 direct blocks. `make` builds
`build/ext2.img`; `run_tests.py` attaches it as a second legacy virtio-blk disk.

## 6. Tests (`kernel/tests/test_ext2.c`)

- **`ext2.reads_a_small_file`** — `/mnt/hello.txt` reads back its exact 19 bytes.
- **`ext2.lookup_through_subdirectory`** — `/mnt/dir/nested.txt` resolves through
  a subdirectory and reads correctly.
- **`ext2.reads_through_indirect_block`** — `/mnt/big.txt` (20000 bytes) reads
  back byte-exact, including offsets past 12 KiB, proving the single-indirect map.

**164 in-kernel tests pass**; `make stress` (serial) is clean.

## 7. Deferred to G2-c

The ext2 **write path** (allocate blocks/inodes, update bitmaps and the
superblock, write directory entries), so programs can create and modify on-disk
files — the last piece before F21 can rebuild MAKH onto a persistent disk.
