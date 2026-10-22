# MAKH Security Advisory — 1.2.0

**Date:** 2026
**Affected:** MAKH 1.1.0 and earlier
**Fixed in:** 1.2.0
**Reporter:** internal review (two static-analysis passes) + KFUZZ v2

All issues below were found **before** 1.2.0 shipped and are fixed in it. None
was ever present in a network-remote-exploitable path: MAKH has no multi-user
accounts and no remote administration, so the realistic attacker is a **local
unprivileged program**, a **corrupt or malicious on-disk ext2 image**, or a
**crafted ELF binary** handed to the loader. Severity is rated in that model
(P1 = kernel compromise or reliable panic from unprivileged input; P2 = resource
exhaustion / leak or a narrower condition; hardening = defense-in-depth).

For how KFUZZ was extended so the fuzzer now reproduces most of these, see
`KFUZZ_V2_STATIC_ANALYSIS.md`.

---

## P1 — unprivileged input compromises or panics the kernel

### AUD-01 — `copy_to_user` store fault panics the kernel
**Where:** `kernel/arch/uaccess.c`, the `__copy_user` fixup.
**Impact:** the uaccess copy loop touches memory twice per byte — a load from
the source and a store to the destination — at two different instruction
pointers. The fault-recovery fixup matched only the **load** RIP. A
`copy_to_user` whose destination was a canonical but read-only or unmapped user
page faulted on the **store**, the fixup didn't recognise the RIP, and the
page-fault handler fell through to a kernel panic. Any syscall that writes back
to a user buffer (and many do) could be turned into a reliable kernel panic by
passing a read-only destination.
**Fix:** `uaccess_fixup` now matches any RIP in the copy-loop body
`[__copy_user_fault_ip, __copy_user_fault)` — load **and** store — and resumes
at the fault label that returns the uncopied count, which becomes `-EFAULT`.
**Regression test:** `test_user.c` — `copy_to_user` to an unmapped canonical
page returns `-EFAULT`, no panic. **Fuzzer:** `t_uaccess` (KFUZZ v2.4).

### AUD-03 — `sigreturn` trusts attacker-controlled state
**Where:** `kernel/signal/signal.c`.
**Impact:** two problems on the signal-return path.
- *(P1)* `sigreturn` restored RIP/RSP straight from the signal context on the
  user stack and returned through `SYSRET`. A user handler could rewrite that
  context to a **non-canonical RIP**; `SYSRET` with a non-canonical RIP raises
  `#GP` **in ring 0, after `swapgs`** — the classic CVE-2012-0217 shape — which
  is a kernel-state compromise, not a clean user fault.
- *(P2)* the restored signal mask was applied verbatim, so a process could mask
  `SIGKILL`/`SIGSTOP` across a `sigreturn` and become **unkillable**.
**Fix:** `signal_restore_ok(rip, rsp)` requires both to be canonical user
addresses before returning; a bad frame terminates the process with
`128+SIGSEGV` instead of returning. The restored mask is forced to clear
`SIGKILL`/`SIGSTOP`, and signal delivery/termination always treat those two as
deliverable regardless of the mask.
**Regression tests:** `test_usignal.c` —
`restore_rejects_noncanonical_state`, `sigkill_delivers_even_if_mask_blocks_it`.
**Fuzzer:** not yet — faithfully fuzzing `sigreturn` needs a ring-3 harness
(see `KFUZZ_V2_STATIC_ANALYSIS.md` §3.5).

### AUD-05 — ext2 superblock geometry: divide-by-zero and shift UB
**Where:** `kernel/fs/ext2.c`, mount.
**Impact:** mount read `s_log_block_size`, `blocks_per_group` and
`inodes_per_group` from an untrusted superblock and used them unchecked.
`block_size = 1024 << s_log_block_size` is undefined when the log is ≥ the word
width, and `blocks_per_group`/`inodes_per_group` are divisors all over the
driver — the first inode lookup computes `(ino-1) / inodes_per_group`, a
divide-by-zero (`#DE`) when that field is 0. A corrupt or hostile ext2 image
could crash the kernel at mount or first access.
**Fix:** `ext2_geometry_ok()` gates mount: it rejects `s_log_block_size > 2`
(MAKH supports ≤ 4 KiB blocks) and any zero group divisor, before any shift or
division runs.
**Regression test:** `test_ext2.c` — `rejects_malformed_geometry`.
**Fuzzer:** `t_ext2geo` (KFUZZ v2.4) — catches both the `#DE` and the shift.

### F1 — ext2 `readdir` name overflow (kernel stack smash)
**Where:** `kernel/fs/ext2.c`, directory iteration.
**Impact:** a directory entry's `name_len` came from disk and was used to copy
the name into a fixed kernel-stack buffer without a bound. A crafted directory
with an over-long name smashed the kernel stack — a classic
disk-data-to-kernel-stack overflow.
**Fix:** the copy is bounded to the destination capacity and the name is always
NUL-terminated in bounds.
**Regression test:** in the ext2 suite. **Fuzzer:** not yet — needs an in-memory
virtio-blk backend to mount mutated images (§3.5).

### F2 — `mprotect`/`munmap` reach kernel page tables
**Where:** `kernel/mm/uvm.c`.
**Impact:** both syscalls accepted any address, including kernel-half and
non-canonical ones, and walked into the **shared kernel** PML4 entries — a user
program could alter or unmap kernel mappings.
**Fix:** both reject any range not wholly inside the user half before touching a
page table.
**Regression test:** mm suite. **Fuzzer:** `t_uvm` adversarial addresses
(KFUZZ v2.2).

---

## P2 — resource exhaustion / leaks / narrower conditions

### F3 — ELF overlapping `PT_LOAD` frame leak
**Where:** `kernel/mm/vmspace.c`, `vmspace_map`.
**Impact:** when two `PT_LOAD` segments mapped onto the same page, `vmspace_map`
overwrote the leaf PTE unconditionally, so the second segment's frame orphaned
the first's — the slot only remembers the last frame, so `vmspace_destroy` frees
that one and the earlier frame leaks. Repeatedly `execve`'ing such a binary is a
slow memory exhaustion.
**Fix:** `vmspace_map` now `page_decref`s the previous frame when it replaces a
present mapping with a *different* frame; a re-map of the same frame (the loader's
W^X re-protect pass) keeps its refcount.
**Fuzzer:** `t_elf` two-segment generator (KFUZZ v2.3).

### F4 — `fork` OOM rollback leak
**Where:** `kernel/mm/vmspace.c`, `vmspace_fork`.
**Impact:** if the page-table copy hit OOM partway, the frames already copied
were not freed.
**Fix:** roll back the partial copy on failure.
**Fuzzer:** `t_oom` fork case under fault injection (KFUZZ v2.1).

### F5 — `kheap_expand` partial-failure leak
**Where:** `kernel/mm/kheap.c`.
**Impact:** heap growth that mapped several pages leaked the earlier ones if a
later map failed.
**Fix:** make expansion atomic — unwind every page mapped so far on failure.
**Fuzzer:** `t_oom` heap case under fault injection (KFUZZ v2.1).

### AUD-07 — ELF loader orphans a frame on a failed mapping
**Where:** `kernel/proc/elf.c`.
**Impact:** the loader allocated a segment frame, then `vmspace_map` failed
(its own intermediate page-table allocation ran out). The segment frame was
mapped nowhere, so `vmspace_destroy` could not reclaim it — a narrow leak on the
map-failure path.
**Fix:** `page_decref` the segment frame on the failed-map path.
**Fuzzer:** ELF loader under fault injection in `t_oom` (KFUZZ v2.4).

---

## Hardening (defense-in-depth)

### AUD-04 — ELF entry point outside any executable segment
**Where:** `kernel/proc/elf.c`.
**Impact:** a malformed `e_entry` could start a process at an address no
`PT_LOAD` executable segment covered.
**Fix:** the loader records whether the (biased) entry lands inside a mapped
`PF_X` segment and returns `-ENOEXEC` otherwise.
**Regression test:** `test_vm.c` — `elf_entry_must_be_in_executable_segment`.

### F6 — `kfuzz_longjmp` re-enabled interrupts unconditionally
**Where:** `kernel/kfuzz/jmp.asm`.
**Impact:** sandbox fault recovery issued an unconditional `sti`, which would
reopen interrupts even if they had been disabled at the matching `setjmp` —
corrupting a critical section the harness was inside.
**Fix:** `longjmp` restores the interrupt flag that `setjmp` captured.
**Regression test:** `test_kfuzz.c` — `longjmp_restores_interrupt_flag`.

---

## Credit where it's due

Static analysis found this batch first. KFUZZ v1 did not. KFUZZ v2 now
reproduces 8 of the 11 with reproducible seeds; the other 3 are fixed and
regression-tested while the fuzzer grows the reach to cover them. The full
accounting — including why the fuzzer was blind to each — is in
`KFUZZ_V2_STATIC_ANALYSIS.md`.
