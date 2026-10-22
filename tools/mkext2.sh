#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# Build the deterministic ext2 test image the ext2 KTEST reads (Phase 20-Q).
# A bare ext2 (1 KiB blocks, 128-byte inodes, no htree/resize features) with a
# few known files, including one >12 KiB file that forces a single-indirect
# block map. Attached to QEMU as a second virtio-blk disk.
set -eu
OUT=${1:-build/ext2.img}
STAGE=$(mktemp -d)

printf 'ext2 works on MAKH\n'        > "$STAGE/hello.txt"
mkdir -p "$STAGE/dir"
printf 'nested-ok\n'                  > "$STAGE/dir/nested.txt"
# 20000 bytes where byte N == '0' + (N % 10): spans >12 blocks at 1 KiB, so the
# ext2 driver must follow the single-indirect pointer to read it all.
python3 -c "import sys; sys.stdout.write(''.join(str(i%10) for i in range(20000)))" > "$STAGE/big.txt"
# Audit F1: ext2 names may be up to 255 bytes, far past MAKH's VFS_NAME_MAX (63).
# A 200-byte name next to a short one lets the KTEST prove readdir never writes
# past the VFS name buffer, whatever the disk holds.
mkdir -p "$STAGE/longdir"
printf 'short\n' > "$STAGE/longdir/short.txt"
printf 'long\n'  > "$STAGE/longdir/$(python3 -c "print('L' * 200)")"
# ...and a directory whose only entry has such a name: it is NOT empty, so rmdir
# must refuse it even though readdir does not list that name.
mkdir -p "$STAGE/longonly"
printf 'only\n' > "$STAGE/longonly/$(python3 -c "print('M' * 120)")"

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
# 2048 blocks with 256 blocks/group => 8 block groups (G2-f): small groups so a
# ~300 KiB (double-indirect) file spills past group 0 into groups 1-2, which
# exercises the driver's multi-group block allocation. -I 128 keeps 128-byte
# inodes; ^resize_inode/^dir_index keep the layout the driver expects.
mke2fs -F -q -b 1024 -g 256 -O ^resize_inode,^dir_index -I 128 -d "$STAGE" "$OUT" 2048
rm -rf "$STAGE"
echo "mkext2: wrote $OUT (8 block groups; hello.txt, dir/nested.txt, big.txt, longdir/)"
