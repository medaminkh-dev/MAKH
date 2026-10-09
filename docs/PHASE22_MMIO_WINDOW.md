# MakhOS Phase 22: a higher-half, shared device-MMIO window

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — device MMIO is mapped in a higher-half window shared into
every address space, so a device register is reachable under any CR3.
**180 in-kernel tests.**
**Depends on:** 21 (higher-half kernel), 14 (PCI / e1000)

---

## 1. Why

`vmm_map_mmio` used to **identity-map** a PCI BAR (virt == phys): a NIC BAR at
`~0xFEB80000` therefore landed in the low canonical half. That was fine while
the kernel owned all of memory, but the higher-half migration (Phase 21) gave
the low half to user processes, and `vmspace_create` shares only the kernel
slots (256..511) into a new address space — **not** the low half. So an MMIO
mapping in the low half was invisible under a user CR3: the kernel could not
read a device register while a user process was the active address space, e.g.
servicing a NIC interrupt that fires mid-syscall. It was latent only because the
test environment attaches no NIC, but it is a real hazard for networking — and a
hard blocker for ever driving a device on behalf of userspace.

## 2. What

A dedicated **MMIO window at PML4 slot 258** (`MMIO_WINDOW_BASE`,
`0xFFFF810000000000`, 512 GiB):

- **`vmm_init` reserves the slot** right after the CR3 switch (so the HHDM is
  live and the new page table can be zeroed through `P2V`). Creating the
  top-level entry *before any user address space exists* means every later
  `vmspace_create` copies it, so the window is shared everywhere.
- **`vmm_map_mmio(phys, size)`** now bump-allocates a virtual range inside the
  window, maps each page there with `PCD|PWT` (uncached, as device registers
  require), and returns the window virtual address (0 on failure). Because the
  PDs/PTs it creates hang under the already-shared slot-258 PDPT, they are
  visible under any CR3.
- **The e1000 driver** uses the returned window address as its register base
  instead of the raw physical/identity address.

The signature changed from `int` (0/-1) to `uint64_t` (the virtual base, 0 on
failure); e1000 is the only caller. virtio-blk is unaffected — it uses an
I/O-port BAR, not MMIO.

## 3. Test (`kernel/tests/test_vm.c`)

The test QEMU attaches no NIC, so the window is exercised directly with a real
RAM frame standing in for a BAR:

- **`mmio.window_is_high_and_shared`** maps a frame via `vmm_map_mmio` and
  checks the returned address is inside the slot-258 window (higher half, not
  the user half); that it resolves back to exactly that frame; that a write
  through the window reaches the frame (read back via the HHDM alias); and that
  a freshly created address space shares the window's top-level PML4 entry — the
  property that makes MMIO reachable under a user CR3.

**180 in-kernel tests pass**; `make stress` (24×4) is clean.

## 4. Next

With the ext2 and MMIO plumbing done, the road turns to broadening userspace:
an interactive shell with real commands, and more syscalls (poll/select, etc.)
so more real programs run unchanged.
