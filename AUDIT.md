# MAKH — Engineering Audit

**Baseline:** `61201e981b68cf1f38a1284a5cc707c10d835a48` (`main`, verified).
**Method:** evidence-first. Read-only (no source changed). Every finding cites file:line, access path, impact, and a reproduction/test. Claims are classified **Confirmed** (proven from code), **Suspected** (plausible path, needs a repro), **Missing** (absent test/feature, no demonstrated defect), **Confirmed-Covered** (already exercised by an existing test — sufficiency assessed, not re-discovered).
**Rule applied:** future/architectural risks are kept out of the current-defect lists (§C). The virtio-blk/ext2 "overflow" was *disproven*, not logged (§F).

---

## 1. Executive summary

The memory-safety core (PMM/VMM/COW/kheap/UVM/ELF/uaccess/ext2-geometry) is **well covered** by KFUZZ v2/v2.5 with fail-first proofs and page-conservation + `kheap_check` oracles (§E). The real gaps are in three areas KFUZZ does **not** reach: **driver liveness** (unbounded PS/2 and RTC polling, blocking inside an IRQ handler), **syscall interruptibility** (`waitpid`), and **resource accumulation over time** (ext2 vnode leak). These map directly onto the oracles UFUZZ 1.4.0 is designed to add (hang/liveness, resource-accounting), which validates that direction.

Most dangerous confirmed items:
- **AUD-L02** — keyboard LED update runs unbounded PS/2 I/O **inside the IRQ handler** (CapsLock path) → system lockup if the controller stalls.
- **AUD-M01** — ext2 vnodes are **never freed**; every ext2 open/lookup on the permanent `/mnt` mount leaks heap → unbounded growth.
- **AUD-P01** — `sys_waitpid` is **non-interruptible** (discards the EINTR return) → a fatal signal can't terminate a process parked in `waitpid`.

The reported **"busybox hang" was not a kernel hang** — root cause was the CI test timeout (240 s) being shorter than the suite's runtime on the slow shared TCG runner (~270 s). Fixed by raising the cap to 900 s; CI is green on `576ae66` (204/204, stress 12×). No scheduler deadlock was found (§C).

---

## 2. Confirmed findings

### AUD-L01 — RTC: unbounded inner wait on UIP  · P0-liveness
- **Where:** `kernel/drivers/rtc.c:39-40`. Outer loop is bounded (`tries < 1000000`), but the inner `while (update_in_progress()) { }` has **no bound and no `io_wait`**.
- **Path:** boot → `ktime_init_realtime()` → `rtc_read_epoch()`.
- **Impact:** if the CMOS UIP bit (reg 0x0A bit 7) never clears, boot hangs forever. (QEMU's RTC clears it, so impact is Suspected; the unbounded loop itself is Confirmed.)
- **Repro/test:** unit-stub `cmos(0x0A)` to keep bit 7 set and assert `rtc_read_epoch` returns within a bounded number of reads.

### AUD-L02 — Keyboard: unbounded PS/2 I/O inside the IRQ handler  · P0-liveness
- **Where:** `keyboard_handler` (IRQ1, `kernel/drivers/keyboard.c:422`) → `keyboard_handle_scancode` → CapsLock case `:327/:329` → `keyboard_set_leds` (`:215`) → `keyboard_send_data` (`:179`, unbounded) + `keyboard_wait_ack` (`:206`).
- **Impact (Confirmed hazard):** the IRQ handler performs **unbounded blocking PS/2 polling with interrupts disabled** on every CapsLock press. If the controller stalls, the whole system locks. It also reads the data port from inside the keyboard IRQ, racing the handler's own scancode consumption.
- **Repro/test:** stub the status port so the input/output-ready bit never sets, deliver a CapsLock scancode to `keyboard_handle_scancode`, assert the handler returns bounded (currently it would not). Proper fix defers LED updates to a non-IRQ context.

### AUD-L03 — Keyboard: polls have no timeout; doc is false  · P0-liveness
- **Where:** `keyboard_send_data` (`:181`), `keyboard_read_data` (`:195`), `keyboard_wait_ack` (`:206-208`). `wait_ack` just calls the unbounded `read_data`; its comment "Returns 1 on ACK, **0 on timeout**" is wrong — **no timeout exists**, so it can never return 0 by timeout. Also on the boot path: `keyboard_init` (`:444-459`).
- **Impact:** an unresponsive PS/2 controller hangs boot/`set_leds` indefinitely.
- **Repro/test:** same stub as AUD-L02; assert bounded return and that `wait_ack` can actually report a timeout.

### AUD-M01 — ext2 vnodes are never freed (permanent-mount heap leak)  · P1-memory
- **Where:** `file_put` frees a vnode **only** for `VNODE_FIFO`/`VNODE_SOCK` at refcount 0 (`kernel/fs/vfs.c:280-285`); `REG`/`DIR` vnodes are never freed. ext2 `make_vnode` allocates a **fresh** vnode+vinfo on every lookup with **no cache** (`kernel/fs/ext2.c:175-187`). `ext2_unmount` (the only freer) is used solely by the private fuzz mount; the real `/mnt` mount (`kernel/kernel.c` boot) is permanent and never unmounts.
- **Impact (Confirmed leak):** every `open`/`stat`/path-lookup that resolves an ext2 inode permanently leaks ~one vnode+`ext2_vinfo`. Long-running ext2 activity → unbounded kernel-heap growth.
- **Access path:** `vfs_resolve`→`resolve_from`→`cur->ops->lookup` (`vfs.c:82`)→`ext2_lookup` (`ext2.c:281`)→`make_vnode`.
- **Repro/test:** loop `fd=vfs_open("/mnt/hello.txt",O_RDONLY); vfs_close(fd);` N=1000 times; assert `kheap_get_used()` does not grow ~linearly with N. (Currently it will.)

---

## 3. Suspected findings (need a reproduction before fixing)

### AUD-P01 — `waitpid` is non-interruptible (EINTR discarded)  · P1
- **Where:** `sys_waitpid` loop calls `sched_wait_event(&cur->child_wq, 0, f)` and **ignores the return value** (`kernel/proc/user.c:569`). `sched_wait_event` returns `2` on signal (`sched.c:341-344`); `signal_send` does wake a `PROC_BLOCKED` target (`signal.c:66-70`), so the wait returns — but `waitpid` just re-scans and re-blocks, never returning `-EINTR` and never reaching the ring-3 signal-delivery path.
- **Impact:** a process blocked in `waitpid` for a child that never exits **cannot be terminated by a signal** (incl. SIGKILL). Diverges from POSIX EINTR semantics. (Confirmed by reasoning; Suspected pending the repro.)
- **Repro/test:** parent `fork`s a child that spins forever, parent calls `waitpid`; send SIGKILL to the parent; assert the parent dies (currently it will not).
- **Contrast (good):** `futex_wait` handles this correctly — `rc==2 → -EINTR` (`kernel/futex.c:60-61`).

### AUD-L04 — `keyboard_get_char` check→`hlt` lost-wake window  · P1-liveness
- **Where:** `kernel/drivers/keyboard.c:478-481`: `while ((c=keyboard_buffer_read())==0) { __asm__ volatile("hlt"); }` — no `cli` before the empty-check and no `sti` paired with `hlt`.
- **Impact:** if an IRQ fills the buffer between the read and the `hlt`, the wake is missed and the CPU halts until the *next* interrupt (self-recovers on the next keypress). Harder failure: if ever entered with IF=0, `hlt` halts permanently.
- **Repro/test:** hard to force the race deterministically; add the standard `cli; if empty {sti; hlt} else sti` guard and a unit check that `get_char` never runs with IF=0.

### AUD-S01 — Scheduler: strict priority, no anti-starvation  · P2 (design)
- **Where:** `kernel/proc/sched/sched.c:68-139`; comment `:131-133` states a lower-priority thread can never preempt a runnable higher-priority one; there is no aging.
- **Impact:** a permanently-runnable higher-priority thread starves all lower ones. Not triggered in the current test mix (equal priority), so no demonstrated hang — design gap.
- **Repro/test:** two threads, high-prio busy-loops, assert the low-prio thread still makes progress.

### AUD-S02 — virtio-blk: ring not reset after a request timeout  · P3
- **Where:** `vblk_rw` bounded poll returns `-EIO` on timeout (`kernel/drivers/virtio_blk.c:207-210`) but does **not** reset the descriptor chain or reconcile `avail`/`used`/`last_used`.
- **Impact:** a late completion from a previously-wedged device could desync the ring on the next request. Limited (one synchronous request at a time), so Suspected/low.
- **Repro/test:** inject a delayed completion after a timeout; assert the next request still matches its own `used` entry.

### AUD-S03 — ext2: untrusted block-pointer error propagation  · P2
- **Where:** `map_block`/`map_indirect` (`kernel/fs/ext2.c:138-180`) read disk block pointers; out-of-range pointers are bounded by the virtio capacity check (`virtio_blk.c:178` → `-EINVAL` → `bread` `-EIO`), and bread failures are treated as sparse/zero. Needs a sweep that **every** `bread` caller checks the return and never consumes an uninitialized block buffer.
- **Impact:** partial image-parse corruption on malformed images (bounded; partly covered by `t_ext2img`).
- **Repro/test:** extend `t_ext2img` mutation to target indirect-block pointers and assert no oracle/guard violation.

---

## 4. Disproven (checked, NOT a finding)

- **virtio-blk / ext2 arithmetic overflow (prior P3-01/02): SAFE.** `bread` computes `(uint64_t)blk * fs->spb` with the cast **before** the multiply (`ext2.c:91`); `blk` is `uint32_t`, `spb ∈ {2,4,8}` (block_size `1024<<log` gated by `ext2_geometry_ok`, AUD-05), so the sector ≤ ~2³⁵ — `sector + count` and `count * 512` (`virtio_blk.c:178,193`) cannot overflow `uint64_t`/`uint32_t`. ChatGPT's pushback was correct. → see Missing-test MT-5 to lock the invariant.

---

## 5. Confirmed-Covered inventory (do not re-audit; sufficiency noted)

| Finding | Covered by | Oracle / proof | Sufficiency |
|---|---|---|---|
| AUD-01 copy_to_user store-fault | `t_uaccess` | caught #PF v14 | strong |
| AUD-03 sigreturn (mask/RFLAGS/canonical) | `/bin/sigfuzz` + white-box KTEST | fail-first flips | strong (RIP/RSP limit documented under TCG) |
| AUD-04 ELF entry outside exec seg | `t_elf` known-verdict | verdict mismatch | strong |
| AUD-05 ext2 geometry | `t_ext2geo` | #DE/shift-UB + oracle | strong |
| F1 ext2 readdir overflow | `t_ext2img` | guard-clobber | strong |
| F2/F3/F5/AUD-07 (mprotect/munmap kernel-VA, ELF overlap, kheap_expand, ELF frame) | `t_uvm`/`t_elf`/`t_oom` | page-conservation | strong |
| F4 fork OOM rollback (`vmspace_fork`) | `t_oom` + `pmm_test_fail_after` (`kfuzz_targets.c:812`) | page-conservation | **assess**: fault injected at some allocation points; broaden injection to each step of `vmspace_fork` to cover "fail at page N" fully |
| kheap/pmm invariants | `t_heap`/`t_pmm` | `kheap_check`/bad_frees | strong |

**Resolved:** busybox "hang" = CI timeout < suite runtime (slow TCG), fixed by 900 s cap; green on `576ae66`. Not a kernel defect.

---

## 6. Missing tests

- **MT-1** Liveness/watchdog tests for driver polls (RTC, keyboard) — none exist; drivers are outside KFUZZ's reach.
- **MT-2** ext2 vnode-accumulation test (AUD-M01 repro).
- **MT-3** `waitpid` interruptibility test (AUD-P01 repro).
- **MT-4** scheduler anti-starvation test (AUD-S01 repro).
- **MT-5** virtio-blk/ext2 arithmetic invariant lock-test (assert `spb∈{2,4,8}`, sector math stays in range).
- **MT-6** futex stress test (correctness already reviewed; no defect found — lock it).

---

## 7. Master task list (priority-ordered)

### P0 — Liveness (current correctness)
| ID | Task | Deps | Acceptance |
|---|---|---|---|
| T-L02 | Move keyboard LED update out of IRQ context (defer to a non-IRQ path); never block in an interrupt handler | — | CapsLock scancode in a stalled-controller stub returns bounded (MT-1) |
| T-L03 | Give `keyboard_send_data/read_data/wait_ack` real timeouts; fix the false "0 on timeout" doc | — | stub makes `wait_ack` return 0 by timeout (MT-1) |
| T-L01 | Bound the RTC inner UIP wait with a try/timeout | — | stuck-UIP stub returns bounded (MT-1) |
| T-P01 | Make `waitpid` honor `sched_wait_event`'s EINTR (return `-EINTR` / allow fatal-signal termination) | — | SIGKILL terminates a `waitpid`-blocked parent (MT-3) |

### P1 — Memory / reliability
| ID | Task | Deps | Acceptance |
|---|---|---|---|
| T-M01 | Stop the ext2 vnode leak: an inode→vnode cache (reuse), or free `REG`/`DIR` vnodes at refcount 0 for ext2 | AUD-M01 | 1000× open/close of an ext2 file: heap growth bounded (MT-2) |
| T-L04 | Fix `keyboard_get_char` lost-wake (`cli`/recheck/`sti`+`hlt`) and guard against IF=0 | — | no halt with IF=0; buffered char never missed |

### P2 — Robustness / design
| ID | Task | Deps | Acceptance |
|---|---|---|---|
| T-S01 | Scheduler anti-starvation (aging or time-slice floor) | AUD-S01 | low-prio thread progresses under high-prio load (MT-4) |
| T-S03 | ext2 `bread`-return audit on all callers; broaden `t_ext2img` to indirect pointers | AUD-S03 | MT-5 + extended fuzz target green |
| T-MT5 | Lock the virtio-blk/ext2 arithmetic invariant with a test | §4 | MT-5 |

### P3 — Lower
| ID | Task | Acceptance |
|---|---|---|
| T-S02 | Reset virtio-blk ring/descriptors after a request timeout | MT: late-completion test |
| T-MT6 | futex stress/correctness test | MT-6 |

### Architectural / future (kept separate — not current defects)
KFUZZ `sb` is a single global sandbox (`kfuzz/kfuzz.c:128`) and `cov_map` is unsynchronized — **correct on single-CPU**, but both are prerequisites to fix before SMP-parallel workers (SMP 2.0.0). `longjmp` recovery should be reviewed for resources held across the jump before parallelism. These belong to SMP/UFUZZ tracks, not to the current-defect list.

---

## 8. Reproduction & test commands

All tests run under the existing harness (`make test`, or `make test-suite SUITE=<name>`), QEMU 8.2.2 TCG, no hardware. Suggested homes:
- MT-1 (drivers): new `kernel/tests/test_driver_liveness.c` with port-stub hooks; assert bounded iterations.
- MT-2 (vnode leak): add to `test_ext2.c` — record `kheap_get_used()`, loop open/close ×1000, assert `used` delta < small bound.
- MT-3 (waitpid): add to `test_fork.c`/`test_usignal.c` — fork a spinner, `kill(parent,SIGKILL)`, assert parent reaped.
- MT-4 (starvation): add to `test_sched.c` — two priorities, assert low-prio progress counter advances.
- MT-5 (arithmetic): add to `test_ext2.c` — assert `spb` and sector math invariants over the mounted image.
Each fix lands with its fail-first proof (revert the fix → the new test fails) and a CI gate, per the project's KFUZZ convention.

---

## 9. Inspection depth (honest coverage of this pass)

- **Fully inspected:** `rtc.c`; `keyboard.c` (polls, IRQ path, `get_char`); `futex.c`; `sys_waitpid`; `signal_send`/`signal_send_pgrp`/wake path; scheduler pick policy (`sched.c` runq/`schedule`/`sched_wait_event`); ext2 `bread`/`map_block`/`spb`/`make_vnode`; VFS `resolve_from`/`file_put`; virtio-blk `vblk_rw`.
- **Partially inspected:** ext2 error-propagation breadth across all `bread` callers; VFS refcount across every op; KFUZZ `sb`/`longjmp`/`cov_map` (reasoned single-CPU-safe, not stress-tested).
- **Not inspected this pass** (deferred, or relied on Confirmed-Covered §5): e1000 RX/TX & init cleanup; PCI config/BAR; TCP state machine, IPv4/UDP parsers; PMM/VMM/kheap/UVM internals (KFUZZ-covered); ELF loader internals (AUD-covered); Multiboot2/boot-map bounds; GDT/TSS/IDT internals.

**Next-highest-value engineering tasks:** T-L02 (IRQ-context blocking) and T-M01 (vnode leak) are the two current defects most worth fixing before serious long-running fuzzing; T-P01 restores expected signal semantics; MT-1/MT-2/MT-3 give the driver-liveness and resource-accounting oracles UFUZZ will build on.
