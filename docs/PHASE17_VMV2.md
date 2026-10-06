# MakhOS Phase 17: Virtual Memory v2 — address spaces, COW, NX/W^X

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — per-process address spaces, copy-on-write with per-frame refcounts, and enforced W^X/NX; 90 in-kernel tests, stress clean
**Depends on:** Phase 16 (ring-3, the user window, the page-fault path)

---

## 1. Scope

This phase builds the **memory machinery** a process model needs: a first-class
address space, reference-counted physical frames, lazy copy-on-write, and
hardware W^X/NX. It delivers the three acceptance tests directly:

- a ring-3 **write to a read-only code page faults** (and executing a data page
  faults) — W^X / NX;
- **copy-on-write fork isolates the child** — a child's write does not touch the
  parent's frame;
- **forking 1000 times leaks nothing** — frame accounting returns to baseline.

Deliberately deferred to **Phase 18** (documented in §7): the buddy and slab
allocators, `mmap`/`munmap`/`brk`, demand paging from a backing store, and the
process-level `fork()` syscall that schedules two real user processes. The COW
engine here is exercised directly at the VMM level, not yet through `fork()`.

---

## 2. Per-frame reference counts (`mm/page.c`)

Copy-on-write means one physical frame is mapped by several address spaces, so
"is this frame free?" is no longer a single bit. `mm/page.c` keeps a `uint16`
refcount per frame in an array sized to actual RAM (~2 bytes per 4 KiB, from
the kernel heap):

```c
void     page_setref(phys, n);   page_incref(phys);
uint32_t page_refcount(phys);    page_decref(phys);  /* frees at 0 via the PMM */
```

`fork` increments; a COW copy or an address-space teardown decrements; the
frame goes back to the PMM only at zero. Raw `pmm_alloc_page`/`pmm_free_page`
callers (heap, page tables, drivers) are untouched — refcounts apply only to
frames that opt in.

---

## 3. Address spaces (`mm/vmspace.c`)

```c
typedef struct address_space { uint64_t pml4_phys; } address_space_t;
```

An address space is one PML4. **Every space shares the kernel's PML4 entries
by value**, so the kernel half (code, heap, identity map) stays mapped under
any CR3; only one PML4 slot — `USER_PML4_INDEX` (64), the Phase-16 user window
at `0x0000_2000_0000_0000` — is private per space.

| | |
|---|---|
| `vmspace_create` | new PML4, copies kernel entries, empty user slot |
| `vmspace_map`    | map a user frame (sets `USER` on the whole walk) |
| `vmspace_fork`   | COW-duplicate the user region |
| `vmspace_cow_fault` | resolve a COW write fault |
| `vmspace_destroy`| drop user frames + free user tables + PML4 |
| `vmspace_switch` | load CR3 |

Page tables are walked through the identity map: every table frame comes from
the PMM (low RAM ≤ 1 GiB, mapped 1:1 by `vmm_init`), so a frame's physical
address is also a valid kernel pointer.

> This keeps the kernel in the lower half for now. A true higher-half kernel
> with a direct map (HHDM) — which lets address spaces share *nothing* in the
> lower half — is a larger relink deferred with SMP (Phase 22).

---

## 4. Copy-on-write fork

`vmspace_fork` deep-copies the user region's **page-table structure** but shares
the **data frames**:

```
for each present user leaf PTE:
    if writable:  clear WRITABLE, set PAGE_COW   (in BOTH parent and child)
    child PTE = parent PTE (read-only, COW)
    page_incref(frame)
```

`PAGE_COW` is a software PTE bit (bit 9). The parent losing write permission is
the whole point: the next write by *either* side faults. If the parent is the
active space, its TLB is flushed so the now-read-only mappings take effect.

`vmspace_cow_fault(as, va)` handles the fault:

- **sole owner** (`refcount == 1`): just clear `PAGE_COW`, set `WRITABLE` — no
  copy;
- **shared**: allocate a fresh frame, copy the page, point this space's PTE at
  the copy (writable, COW cleared), `page_decref` the old frame.

So a child's write gives the child a private page and leaves the parent's frame
untouched — and vice-versa.

---

## 5. W^X and NX

`vmm_init` enables `EFER.NXE` (after checking CPUID `0x80000001` EDX[20]). The
page mapper now preserves the NX bit (bit 63) through to the leaf PTE — it used
to be masked off. The Phase-16 user harness maps accordingly:

- **code page**: present + user, **not writable**, executable → RX;
- **stack/data page**: present + user + writable + **NX** → RW, non-exec.

A ring-3 write to the code page, or an instruction fetch from the stack, raises
`#PF` (vector 14), which the Phase-16 handler contains as `USER_FAULTED`.

---

## 6. Tests

`kernel/tests/test_vm.c`:

- **`page_refcount_frees_on_last_put`** — incref/decref accounting; the frame
  returns to the PMM exactly at zero.
- **`cow_fork_isolates_child_writes`** — map `0xAA`, fork, COW-fault the child;
  the child moves to a new frame, the parent keeps the original, and writing
  `0xBB` into the child's copy leaves the parent's `0xAA` intact. Frees cleanly
  (no leak).
- **`fork_many_times_without_leak`** — fork + destroy 1000×; the shared frame's
  refcount cycles 1→2→1 and total free RAM returns exactly to baseline.

`kernel/tests/test_user.c` (W^X):

- **`write_to_rx_code_faults`** — ring-3 write to the RX code page → `#PF`.
- **`execute_nx_stack_faults`** — ring-3 jump into the NX stack → `#PF`.

**91 in-kernel tests pass**; `make stress` is clean. The COW engine is also
fuzzed: the KFUZZ `vmspace` target throws random create/map/fork/cow_fault/
destroy sequences at it and asserts the PMM returns to baseline after each
run (a leak or double-free breaks that balance and is reported with the
reproducing seed). A 40,000-iteration campaign (~1.28M operations) found zero
crashes and zero accounting failures.

---

## 7. Deferred to Phase 18+

| Item | Why it waits |
|---|---|
| Buddy + slab allocators | the bitmap PMM + heap are sufficient for now; COW needs refcounts, not a new allocator |
| `mmap`/`munmap`/`brk` | need the VMA list and a process to own it |
| VMAs (regions) | pair naturally with `mmap` and demand paging |
| Demand paging | needs a backing store (a filesystem — Phase 18) |
| process-level `fork()` | needs two schedulable user processes, each with its own `address_space_t` + CR3 on context switch |
| Higher-half kernel (HHDM) | a boot/relink change, batched with SMP (Phase 22) |

The engine those features drive — reference-counted frames and COW address
spaces — is in place and tested.
