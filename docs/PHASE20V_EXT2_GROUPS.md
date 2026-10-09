# MakhOS G2-f: ext2 multi-block-group allocation

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the ext2 driver now allocates and frees blocks and inodes
in **every block group**, not just group 0. **179 in-kernel tests.**
**Depends on:** 20-Q/R/T (ext2 read/write/dirops), 20-U (indirect blocks)

---

## 1. Scope

ext2 divides a volume into fixed-size *block groups*, each with its own block
and inode bitmaps and inode table. The write path so far allocated only from
group 0: once group 0's blocks or inodes were used up, the driver reported the
filesystem full even though later groups had room — a hard cap at one group's
worth of space regardless of the real volume size. This brick makes allocation
and freeing span all groups.

Reading inodes was already group-aware (any file's inode is found via its
group's descriptor); only the allocator was group-0-bound.

## 2. How it works

A small set of group-indexed helpers replaces the group-0 hardcoding:

- `group_count()` — how many groups the volume has.
- `gd_read32(g, off)` — read a field (block bitmap / inode bitmap / inode
  table) from group `g`'s 32-byte descriptor, indexing the GDT by byte so it
  works when the descriptor table spans several blocks.
- `adj_free_counts(g, …, delta)` — adjust group `g`'s free-block/‑inode counter
  and the superblock's mirror together, by ±1.

With those:

- **`alloc_block`** scans groups in order, trying each group's block bitmap
  (bounded by that group's block count — the last group may be short) until one
  yields a free bit; the absolute block number is
  `first_data_block + g·blocks_per_group + bit`.
- **`alloc_inode`** scans groups' inode bitmaps; the inode number is
  `g·inodes_per_group + bit + 1`.
- **`free_block` / `free_inode_num`** compute the owning group from the block
  or inode number (`(bn−first_data_block)/blocks_per_group`,
  `(ino−1)/inodes_per_group`), clear the bit in that group's bitmap and bump
  that group's free counter.

## 3. Test (`kernel/tests/test_ext2.c` + `tools/mkext2.sh`)

The test image is now built with `mke2fs -g 256` over 2048 blocks → **8 block
groups** of 256 blocks / 32 inodes each, so allocation has to leave group 0 to
succeed:

- **`ext2.multi_block_group_alloc`** creates 25 files — more than group 0's ~17
  free inodes — so `alloc_inode` must move into group 1+, and writes a byte to
  each (a data block from `alloc_block`). It then reads every file back from its
  on-disk inode and checks the byte, proving the cross-group inode/block math.
- **`ext2.double_indirect_large_file`** (from G2-e) now also spans groups: its
  ~293 data blocks overflow group 0's ~211 free blocks into groups 1-2.

**179 in-kernel tests pass**; `make stress` (24×4) is clean.

## 4. Next

The pending plumbing list continues with the MMIO high window (for exposing
networking to userspace); after that, broadening userspace (shell + commands,
more syscalls).
