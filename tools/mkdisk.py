#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# Build the deterministic test disk image the virtio-blk KTEST reads/writes
# (Phase 20-P). Sector 0 carries a magic + a 32-bit sentinel + a known byte
# pattern the kernel verifies; the image is attached to QEMU as a legacy
# virtio-blk disk with snapshot=on, so writes are ephemeral and parallel-safe.
import struct, sys

SECTOR = 512
SECTORS = 2048                      # 1 MiB
MAGIC = b"MAKHDSK1"                 # offset 0, 8 bytes
SENTINEL = 0xC0FFEE42               # offset 8, 32-bit LE

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "build/testdisk.img"
    img = bytearray(SECTOR * SECTORS)
    s0 = bytearray(SECTOR)
    s0[0:8] = MAGIC
    s0[8:12] = struct.pack("<I", SENTINEL)
    for i in range(16, SECTOR):     # a checkable ramp in the rest of sector 0
        s0[i] = i & 0xFF
    img[0:SECTOR] = s0
    with open(out, "wb") as f:
        f.write(img)
    print(f"mkdisk: wrote {out} ({SECTORS} sectors, magic+sentinel in sector 0)")

if __name__ == "__main__":
    main()
