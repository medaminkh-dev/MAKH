# MakhOS Phase 20-P (G2-a): persistent storage — a virtio-blk disk

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — MAKH reads and writes a real disk in 512-byte sectors
over a polled legacy virtio-blk device. **161 in-kernel tests.**
**Depends on:** Phase 14 (PCI), the PMM (bitmap allocator)

---

## 1. Scope

Everything so far has lived in RAM (tmpfs + the tar initrd). This is the first
brick of **gap #2, persistent storage**: a block device MAKH can read and write,
the substrate a real on-disk filesystem (ext2, G2-b) will sit on.

- A minimal **legacy virtio-blk** driver (virtio-pci 0.9.5 / transitional),
  polled — no interrupts. Legacy is the simpler transport (control registers in
  an I/O-port BAR, a fixed device-chosen queue size), enough to move sectors.
- **Read and write** of N contiguous sectors, synchronous.
- **Deferred:** a filesystem on top (ext2), a block cache, interrupt-driven/async
  requests, and multiple queues — none needed to prove the device works.

---

## 2. A contiguous page allocator (`mm/pmm.c`)

A legacy split-virtqueue is one physically-contiguous region whose address is
handed to the device as a single page-frame number, and for the device's queue
size (QEMU gives 256) it spans three pages. The PMM only had single-page alloc,
so this brick adds **`pmm_alloc_pages(n)`**: a first-fit scan of the bitmap for a
run of `n` free pages (and `pmm_free_pages`). The e1000 DMA invariant holds here
too — PMM frames are identity-mapped, so the region's virtual address is its
physical (DMA) address, and its PFN is just `addr >> 12`.

## 3. The driver (`drivers/virtio_blk.c`)

Bring-up follows the legacy handshake: reset → ACK → DRIVER → negotiate
(we accept no optional features) → publish the virtqueue's PFN → DRIVER_OK, then
read the capacity from device config. One split virtqueue is laid out in the
contiguous region (`desc | avail | pad-to-4096 | used`), sized from the device's
`QUEUE_NUM`.

Each request is a three-descriptor chain — a 16-byte header (device-readable),
the data buffer (device-writable for a read, readable for a write), and a
one-byte status (device-writable) — published into the available ring, kicked
with a notify, and completed by **polling the used ring**. The data buffer must
be a physically-contiguous, identity-mapped kernel buffer (a PMM page), matching
the DMA model the NIC already uses.

## 4. The bug this surfaced: a legacy INTx storm

The first request wedged the machine mid-way through the very next line of
output. Root cause: legacy virtio signals completion with a **level-triggered**
PCI interrupt, and a purely polling driver never reads the ISR to deassert it —
so once the device raised INTx, it stayed asserted and the PIC re-fired it
forever, starving everything. The fix is the correct one for a polled driver:
set **`VRING_AVAIL_F_NO_INTERRUPT`** in the available ring so the device does not
raise a completion interrupt at all. (A later, interrupt-driven version would
instead install a handler that reads the ISR to ack it.)

## 5. The test disk (harness)

`tools/mkdisk.py` writes a deterministic 1 MiB image: sector 0 carries a magic
(`MAKHDSK1`), a 32-bit sentinel (`0xC0FFEE42`), and a byte ramp. `make` builds it
to `build/testdisk.img`, and `tools/run_tests.py` attaches it as a **legacy**
virtio-blk device (`disable-modern=on`) with **`snapshot=on`** — so writes go to
an ephemeral overlay and parallel stress runs never corrupt the backing image or
each other.

## 6. Fuzzed

No new KFUZZ target this brick: the driver's inputs are the kernel's own
sector/buffer arguments, not untrusted data (an untrusted on-disk *filesystem*
is the fuzz surface that arrives with ext2, G2-b). The request path is validated
instead by the read/write KTESTs and by the bounded poll (a wedged device
returns `-EIO` rather than hanging).

## 7. Tests (`kernel/tests/test_virtio_blk.c`)

- **`vblk.reads_known_sector0`** — capacity is 2048 sectors; reading sector 0
  reproduces the magic, the 32-bit sentinel, and the byte ramp exactly.
- **`vblk.write_read_roundtrip`** — writing a known pattern to a scratch sector
  and reading it back returns the same 512 bytes (ephemeral under snapshot=on).

**161 in-kernel tests pass**; `make stress` (serial) is clean.

## 8. Deferred to G2-b

An **ext2** filesystem over this device (superblock, block groups, inodes,
directory walk, read — then write), mounted into the VFS so programs open real
on-disk files. That is what lets F21's compiler read sources and write objects
to a disk that survives a reboot.
