# MakhOS Phase 20-B: anonymous memory — brk & mmap

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE (first brick) — `brk`, anonymous `mmap`/`munmap`, and
`mprotect` for ring-3 processes; 125 in-kernel tests, fuzzed, leak-checked
**Depends on:** Phase 17 (address spaces, NX/W^X), Phase 20-A (user processes)

---

## 1. Scope

A userland allocator needs the kernel to hand it zeroed, anonymous memory and
take it back. This phase adds exactly that for a ring-3 process:

- **`brk(addr)`** — move the program break (the classic heap edge);
- **`mmap(len, prot, MAP_ANONYMOUS|MAP_PRIVATE)`** — a fresh zeroed region;
- **`munmap(addr, len)`** — return a region's frames;
- **`mprotect(addr, len, prot)`** — change a region's protection (W^X).

It is the smallest surface that malloc (eventually musl's) stands on.

**Deferred:** file-backed `mmap` (needs the page cache), shared mappings
(`MAP_SHARED`), `MAP_FIXED` placement, and address-range *reuse* — the mmap
arena here is a bump allocator, so an `munmap`'d range frees its frames but its
address is not recycled. The 512 GiB private window makes that a non-issue for
now; a real VMA tree replaces it when the surface grows.

---

## 2. Two new address-space primitives (`mm/vmspace.c`)

Phase 17 could map a frame and resolve a COW fault; Phase 20-B adds the two
operations the syscalls need:

- **`vmspace_unmap(as, va)`** — clear the leaf PTE and drop a reference to its
  frame (freeing it on the last reference); flush the TLB entry if `as` is the
  active space.
- **`vmspace_protect(as, va, flags)`** — change a leaf's `WRITABLE`/`NX` bits,
  keeping the frame. A **COW page is never force-made-writable**: it stays COW
  so a later write still takes a private copy, which preserves fork isolation
  (relevant once user `fork` lands in 20-A-2).

## 3. The engine (`mm/uvm.c`)

The four operations are written as **pure functions over an address space plus
cursors** — a program break and an mmap bump pointer — so they can be tested
directly and fuzzed, with no process object in sight:

```c
long uvm_brk  (as, &brk_cur, brk_start, newbrk);   /* grow/shrink/query */
long uvm_mmap (as, &mmap_cur, len, prot, flags);   /* -> base addr, or -errno */
int  uvm_munmap(as, addr, len);
int  uvm_mprotect(as, addr, len, prot);
```

Anonymous frames are **zero-filled on allocation** (no information leak from a
freed page). `mmap` is **all-or-nothing**: if a frame or page-table page can't
be allocated partway through, every page already mapped by that call is rolled
back before it returns `-ENOMEM`. `prot` maps to PTE bits with a W^X-friendly
default — a page is non-executable unless `PROT_EXEC` is asked for.

**Layout inside the private window** (`PML4[64]`, `[0x2000_0000_0000 …
0x2080_0000_0000)`): the program sits near the base and the stack just above
it; the **heap** (brk) starts 32 MiB in and the **mmap arena** 1 GiB in, so the
two grow toward higher addresses without ever meeting the program, the stack,
or each other.

## 4. Syscalls (`kernel/syscall/syscall.c`)

`mmap`(9), `mprotect`(10), `munmap`(11) and `brk`(12) match the Linux x86-64
numbers. Each wrapper operates on the **calling process's** address space and
cursor fields (`aspace`, `brk_cur`, `mmap_cur` in the PCB) and returns
`-ENOSYS` for a non-user caller. `mmap` needs a fourth argument (`flags`), so
the dispatcher now also forwards `r10` — the syscall ABI's stand-in for `rcx`,
which the `syscall` instruction clobbers.

## 5. Fuzzed (`uvm` target)

A KFUZZ **`uvm`** target drives a random stream of mmap/munmap/mprotect/brk
against one throwaway space, tracking live regions so it can unmap them. The
oracle is strict **page conservation** across the campaign — create → random
ops → destroy must return every frame — so a leak (a rolled-back mmap that
forgot a frame, a munmap that failed to free) or a double free is caught at
once. The all-targets campaign includes it; a focused campaign is clean.

## 6. Tests (`kernel/tests/test_uvm.c`)

Engine level, with a net-zero page oracle around each case:

- **`mmap_maps_zeroed_pages_then_munmap_frees`** — three pages map, read back as
  zero, unmap, and free memory returns to where it started.
- **`brk_grows_and_shrinks_cleanly`** — grow four pages, shrink to empty, no leak.
- **`mmap_rejects_bad_requests`** — zero length and non-anonymous are `-EINVAL`.
- **`mprotect_leaves_the_page_mapped`** — the page stays mapped after a prot change.

End to end, through real ring-3 programs in the initrd:

- **`user_mmap_roundtrip`** (`/bin/vmtest`) — mmap a region, stamp and verify a
  pattern, unmap it, then grow `brk` and use the new page; returns 0 iff all held.
- **`user_mprotect_read_only_faults_on_write`** (`/bin/mprotfault`) — mmap RW,
  write, drop to `PROT_READ`, write again → SIGSEGV (status 139). This proves
  `mprotect` takes effect (TLB flushed) and W^X is enforced in ring 3.

**125 in-kernel tests pass**; `make stress` is clean.

## 7. Deferred

| Item | Why it waits |
|---|---|
| file-backed `mmap` | needs the page/buffer cache |
| `MAP_SHARED`, `MAP_FIXED` | shared frames and fixed placement; needs a VMA tree |
| mmap address reuse | the arena is a bump allocator today (frames are freed, addresses are not) |
| `fork` inheriting the heap/mmap regions | arrives with user `fork` in Phase 20-A-2 |

These, with the rest of the syscall surface (`pipe`, `dup`, `ioctl`,
`getdents`, …) and per-process cwd, are what still stand between here and a
musl/busybox userland.
