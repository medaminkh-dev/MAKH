# MakhOS G2-e: ext2 double- and triple-indirect block maps

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the ext2 driver now maps, allocates and frees files
through the **double- and triple-indirect** block pointers, not just direct +
single-indirect. **178 in-kernel tests.**
**Depends on:** 20-Q/R/T (ext2 read, write, directory ops)

---

## 1. Scope

Until now the ext2 driver followed only the 12 direct pointers and the single-
indirect block — a ceiling of 12 + 256 = 268 blocks, i.e. ~268 KiB at 1 KiB
blocks (~4 MiB at 4 KiB). This brick adds the remaining two levels of the
classic ext2 block map:

- **double-indirect** (`i_block[13]`): a block of pointers to single-indirect
  blocks → +`per²` blocks (64 MiB at 1 KiB, 1 GiB at 4 KiB);
- **triple-indirect** (`i_block[14]`): a block of pointers to double-indirect
  blocks → +`per³` blocks (16 GiB at 1 KiB, 4 TiB at 4 KiB).

So on-disk files are no longer capped at the single-indirect limit — the "feel
restricted" ceiling on file size is gone.

## 2. How it works

All three indirect levels are the same structure at different depths, so the
driver handles them with one recursive routine per operation rather than three
hand-unrolled copies (`per` = pointers per block = block_size/4):

- **read** — `map_block` subtracts the direct and each indirect span in turn,
  then `map_indirect(ptr_blk, level, idx)` walks down: at level 1 it reads the
  data pointer at `idx`; deeper, it reads the child pointer at `idx / perˡ⁻¹`
  and recurses on `idx % perˡ⁻¹`.
- **write** — `alloc_indirect(&root, level, idx)` mirrors that walk, allocating
  any missing pointer block along the path (each `alloc_block` returns a zeroed
  block) and wiring the fresh data block into the leaf; interior slots are
  written back only when the recursion created a child.
- **truncate** — `free_indirect_from(&root, level, start)` frees every entry
  with within-span index ≥ `start`: a child subtree wholly past `start` is
  freed outright and its slot cleared, the one child that straddles `start` is
  recursed into and kept, and the root pointer block itself is released only
  when `start == 0` (nothing survives). `i_blocks` is kept in step throughout.

## 3. Test (`kernel/tests/test_ext2.c`)

- **`ext2.double_indirect_large_file`** — writes a 300 000-byte file (≈293 blocks:
  blocks 268.. live in the double-indirect tree), re-resolves it straight from
  the on-disk inode, reads it all back and checks the pattern — including the
  first double-indirect byte (274 432) and the last byte. Then truncates it to
  5 000 bytes and confirms the surviving data is intact, exercising
  `free_indirect_from` releasing the whole double-indirect tree.

Triple-indirect only engages past ~64 MiB (1 KiB blocks), too large for the
1 MiB test image, so it is covered by construction: it is the exact same
recursion one level deeper, driven by the same code the double-indirect test
exercises.

**178 in-kernel tests pass**; `make stress` (24×4) is clean.

## 4. Next

`G2-f`: multi-block-group allocation, so blocks and inodes come from every
block group (not only group 0) — larger filesystems, not just larger files.
