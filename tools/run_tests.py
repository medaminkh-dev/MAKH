#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
"""
run_tests.py - Boot a MakhOS test ISO in QEMU and report the result.

The kernel, booted with "makh.test", runs its in-kernel test suite and then
powers off through QEMU's isa-debug-exit device:

    qemu exit status = (kernel_code << 1) | 1

so kernel_code 0 (all tests passed) => status 1, and kernel_code 1 (failures)
=> status 3. A kernel that triple-faults with -no-reboot, or hangs, is treated
as a failure. The kernel's serial output is streamed to stdout so failures are
visible in CI logs.

Usage: run_tests.py <test-iso> [--timeout SECONDS]
"""

import subprocess
import sys
import os

QEMU = "qemu-system-x86_64"
SUCCESS_STATUS = 1  # kernel called qemu_debug_exit(0)


def main():
    if len(sys.argv) < 2:
        print("usage: run_tests.py <test-iso> [--timeout N]", file=sys.stderr)
        return 2

    iso = sys.argv[1]
    timeout = 60
    if "--timeout" in sys.argv:
        timeout = int(sys.argv[sys.argv.index("--timeout") + 1])

    if not os.path.exists(iso):
        print(f"error: test ISO not found: {iso}", file=sys.stderr)
        return 2

    cmd = [
        QEMU,
        "-cdrom", iso,
        "-m", "256M",
        "-display", "none",
        "-serial", "stdio",
        "-no-reboot",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        # TCG keeps this runnable on CI hosts without KVM.
        "-accel", "tcg",
    ]

    # Phase 20-P: attach the test disk as a LEGACY virtio-blk device when it
    # exists (built by `make`). snapshot=on makes writes ephemeral, so the
    # KTEST can write+read-back and parallel stress runs never corrupt it.
    disk = os.path.join(os.path.dirname(iso) or ".", "testdisk.img")
    if not os.path.exists(disk):
        disk = "build/testdisk.img"
    if os.path.exists(disk):
        cmd += [
            "-drive", f"file={disk},if=none,id=vblk0,format=raw,snapshot=on",
            "-device", "virtio-blk-pci,drive=vblk0,disable-modern=on,disable-legacy=off",
        ]
    # A second legacy virtio-blk disk holding an ext2 image (Phase 20-Q). The
    # ext2 driver finds it by scanning units for the ext2 magic, so order is
    # not load-bearing. snapshot=on keeps it read-only in effect.
    ext2 = os.path.join(os.path.dirname(iso) or ".", "ext2.img")
    if not os.path.exists(ext2):
        ext2 = "build/ext2.img"
    if os.path.exists(ext2):
        cmd += [
            "-drive", f"file={ext2},if=none,id=vblk1,format=raw,snapshot=on",
            "-device", "virtio-blk-pci,drive=vblk1,disable-modern=on,disable-legacy=off",
        ]

    print("=== MAKH test runner ===")
    print("qemu:", " ".join(cmd))
    print(f"timeout: {timeout}s\n", flush=True)

    try:
        proc = subprocess.run(cmd, timeout=timeout, capture_output=True, text=True)
        status = proc.returncode
    except subprocess.TimeoutExpired as e:
        sys.stdout.write(e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or ""))
        print("\n=== TIMEOUT: kernel did not power off in time (likely a hang) ===",
              file=sys.stderr)
        return 1

    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)

    print(f"\n=== qemu exit status: {status} ===")
    if status == SUCCESS_STATUS:
        print("=== RESULT: PASS ===")
        return 0

    kernel_code = (status - 1) >> 1 if status > 0 else "?"
    print(f"=== RESULT: FAIL (kernel exit code {kernel_code}) ===", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
