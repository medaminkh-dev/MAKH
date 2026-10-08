# MakhOS Phase 21 (F21 Path A): the higher-half kernel migration

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the kernel executes entirely from the higher half, the
whole lower canonical half belongs to each user process, and a standard non-PIE
executable loads and runs at its native low address (0x400000). **173 in-kernel
tests.**
**Depends on:** 20-T (and the whole ring-3 userland arc, 16–20)

---

## 1. Scope

Until now the kernel was identity-mapped low (linked at 0x100000) and every
process address space *shared* the kernel's low PML4 entries — so the entire
lower canonical half was really the kernel's identity map, and a user program
could only live in one carved-out slot (PML4 slot 64, 0x200000000000). A static
PIE fits there with a load bias, but a **standard non-PIE executable** — what a
stock `gcc`/`tcc` emits, fixed at 0x400000 with the small code model — could
not: 0x400000 was kernel territory.

F21's self-hosting goal (port a C compiler, assemble, link, run `make`) needs
exactly that: run the toolchain's own output. So this phase moves the kernel out
of the way. It is a classic **higher-half kernel**: the kernel is relinked into
the top -2 GiB, reaches all physical memory through a **higher-half direct map
(HHDM)**, and user address spaces stop sharing the low half — freeing slots
0..255 for programs at their native addresses.

The migration landed as four bricks, each keeping the full test suite green and
`make stress` clean so a regression never reached `main`:

| Brick | What |
|---|---|
| A1 | Add the HHDM (PML4 slot 256) alongside the legacy identity map |
| A2 | Route **every** physical-frame access through the HHDM (`P2V`) |
| A3 | Relink the kernel into the higher half (`-mcmodel=kernel`) |
| A4 | Free the lower half from user spaces; load standard low binaries |

Deferred: device MMIO (e1000 BARs) is still identity-mapped, so it is reachable
only under the kernel's master CR3 (boot and kernel threads), not a user CR3.
That is fine today — nothing lets a user process drive the NIC directly — and
moves to a high MMIO window when user-initiated networking lands.

## 2. The two half-maps (`mm/vmm.h`)

Two independent linear offsets, each with its own inverse, keep "physical
frame" and "kernel symbol" cleanly separated:

- **HHDM** — `HHDM_BASE = 0xFFFF800000000000` (slot 256). `P2V(phys)` is the
  kernel's window onto any physical frame; `V2P` its inverse. The HHDM is shared
  into every address space, so a frame is reachable no matter which CR3 is live.
- **Kernel link bias** — `KERNEL_VMA_BASE = 0xFFFFFFFF80000000` (slot 511, the
  `-mcmodel=kernel` region). A kernel symbol's address is its physical load
  address plus this bias; `KV2P()` recovers the physical for CR3 and for the
  page-table entries that point at kernel-static tables.

## 3. A1 — the direct map (`mm/vmm.c`)

`vmm_init` builds a PDPT + a PD of 2 MiB huge pages covering RAM at `HHDM_BASE`,
in addition to the existing low identity map. `vmm_alloc_page`'s bump region
moves to slot 257 so a 1 GiB direct map never collides with it. Purely additive:
nothing uses the HHDM yet, so all tests stay green. `test_mm.c` gains a KTEST
proving `P2V()` aliases an independent mapping of the same frame.

## 4. A2 — everything onto the HHDM (`mm/vmm.c`, `mm/vmspace.c`, drivers, loaders)

Every site that treated a physical address as a pointer now goes through
`P2V()`:

- page-table walks (`get_or_create_*`, `vmm_unmap_page`, `vmm_get_physical`,
  and `vmspace.c`'s `tbl()`);
- ext2 block buffers (`blk_alloc`/`blk_free`: a PMM frame held as an HHDM
  pointer, freed by physical; the virtio layer translates back for DMA);
- virtio-blk and e1000 DMA structures (HHDM pointers for the CPU,
  `vmm_get_physical()`/saved PFN for the device);
- process-image setup in `elf.c`, `user.c`, `uvm.c`, `usermode.c`.

The low identity map stays as a working alias throughout A2, so the system keeps
booting while the data plane migrates. **A latent bug surfaced here**: a user
fault on a low kernel address walks the shared slot-0 identity *huge* pages, and
the old walk only treated that as "not a user page" by the accident that
physical 0 cast to the NULL pointer. The HHDM removes that accident, so
`ensure()` now refuses to descend into a huge page explicitly — otherwise a
stray `*(int*)0x1234` was mis-resolved as a copy-on-write page instead of
becoming `SIGSEGV`.

## 5. A3 — relink into the higher half (`linker.ld`, `boot/boot.asm`, `Makefile`)

The kernel is built with `-mcmodel=kernel` and linked at `KERNEL_VMA_BASE`.
`linker.ld` keeps a small **low `.boot`** section (the multiboot header and the
32-bit trampoline, which run with paging off) linked 1:1 at 0x100000, and places
the rest at `KERNEL_VMA + physical` but `AT()`-loaded low — so GRUB still drops
one contiguous image at 0x100000.

`boot.asm` maps a higher-half window (PML4[511]→PDPT[510]→PD, 64 MiB) beside the
low identity map, enables long mode, far-jumps to a 64-bit low stub, then
`mov rax,<high entry>; jmp rax` into `.text` at `KERNEL_VMA`. `vmm_init` uses
`KV2P()` for CR3 and the HHDM table entries; the never-used slot-511 recursive
self-map is gone (slot 511 is the kernel now). Confirmed live: `kernel_main`
runs at `RIP = 0xFFFFFFFF80...`.

## 6. A4 — free the lower half; run standard low binaries (`mm/vmspace.c`, `proc/elf.c`)

- `vmspace_create` shares only the kernel's higher-half slots (256..511); the
  whole lower half (0..255) is the process's own. `vmspace_fork` and
  `vmspace_destroy` copy/free **every** populated user slot, not just slot 64.
- `elf.c`'s load window widens from the single slot-64 region to the entire
  lower half `[page 1, kernel-half start)`; the PIE bias stays at slot 64,
  decoupled from the window floor. Page 0 stays reserved so a NULL deref faults.
- VGA is addressed through the HHDM (`0xFFFF8000000B8000`, mapped early by
  `boot.asm`), so the `write()` syscall path reaches the screen under a user CR3
  that no longer carries slot 0. The initial/idle kernel stack moves off the low
  boot stack (0x90000) into the kernel image's `.bss` (higher half, shared), so
  the idle thread's context switches survive running under a user CR3.
- Two last identity dereferences — the initial-stack writer (`poke`) and the
  Phase-16 code loader — go through the HHDM. They surfaced as a fork/execve
  triple-/page-fault and were fixed.

## 7. Fuzzed

No new KFUZZ target. The existing **`elf`** target hammers malformed ELFs
through the now-wider load window; the bounds checks (`USER_SEG_MAX`, the
overflow-safe window arithmetic) still cap the work, so a hostile ELF is
contained to the process's own lower half. The **page-fault/COW** paths are
exercised constantly by the fork and signal suites under the slot-0-free user
CR3.

## 8. Tests (`kernel/tests/test_proc.c`, `kernel/tests/test_mm.c`)

- **`vmm.hhdm_aliases_physical_frame`** — `P2V()` aliases an independent mapping
  of the same frame and lands in the higher half.
- **`proc.standard_low_binary_runs`** — spawns `/bin/lowexec`, a non-PIE
  `ET_EXEC` linked at **0x400000** (`user/user_low.ld`), and asserts it prints
  its marker (a `write()` under its slot-0-free CR3) and exits 77.
- The whole prior suite — fork/COW, signals, musl and busybox static-PIEs at
  slot 64, ext2 on virtio-blk — now runs with the kernel in the higher half and
  the low half owned per-process.

**173 in-kernel tests pass**; `make stress` (24×4) is clean.

## 9. Next

The low half is open, so F21 proper resumes: a self-hosting toolchain — get a C
compiler's low ET_EXEC output running on MAKH, then assemble + link + `make`,
and ultimately rebuild MAKH on itself. The deferred high MMIO window is the
prerequisite for user-space networking, whenever that is wanted.
