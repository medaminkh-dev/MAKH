# MakhOS Phase 16: Ring-3 userspace foundation

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — real ring-3 execution, a correct `syscall`/`sysret` ABI, safe user-memory access, and fault containment; 85 in-kernel tests, stress + smoke clean
**Depends on:** Phases 11–15 (test harness, scheduler, heap, exceptions)

---

## 1. What this phase is (and is not)

Phase 16 is the **trusted machinery** for running code in ring 3: the GDT/MSR
setup, the entry/exit trampolines, the system-call ABI, safe access to user
memory, and containment of faults that happen in user mode. It is demonstrated
by tiny hand-assembled user programs that run in ring 3, make system calls, and
(deliberately) fault.

It is **not** yet the full process model. ELF loading, `execve`, `fork`, and
per-process address spaces need the VMM rework and a loader; those are
**Phase 17**. Phase 16 runs one user program at a time, in the single shared
address space, non-preemptively. Everything here is the layer the process model
will sit on.

---

## 2. The GDT and the `syscall`/`sysret` contract

`sysret` does not take a target selector; it derives the ring-3 selectors from
`STAR[63:48]`:

```
sysretq:  SS = STAR[63:48] + 8   (RPL 3)
          CS = STAR[63:48] + 16  (RPL 3)
```

So the GDT must place **user data 8 bytes below user code**. The layout is now:

| Sel  | Entry | Meaning            |
|------|-------|--------------------|
| 0x08 | code  | kernel code (ring0)|
| 0x10 | data  | kernel data (ring0)|
| 0x18 | data  | **user data**  (ring3) |
| 0x20 | code  | **user code**  (ring3) |
| 0x28 | TSS   | 128-bit descriptor |

`STAR = (0x10 << 48) | (0x08 << 32)`:
- `syscall` → CS 0x08, SS 0x10 (ring 0);
- `sysret` → SS 0x18|3 = 0x1B, CS 0x20|3 = 0x23 (ring 3).

(The previous code had the user code/data order reversed and `STAR` halves
swapped — harmless only because the `syscall` instruction had never actually
been used from ring 3.)

---

## 3. Entry and exit (`kernel/arch/usermode_asm.asm`)

- **`enter_user_mode(rip, rsp)`** builds an `iretq` frame (`SS, RSP, RFLAGS,
  CS, RIP`), zeroes every GPR so no kernel value leaks to ring 3, and `iretq`s.
- **`syscall_entry`** is the `IA32_LSTAR` target. `syscall` does *not* switch
  stacks, so it `swapgs`es to the per-CPU block, loads the kernel stack from it,
  saves a full **trapframe**, and calls `syscall_dispatch(tf)`. The dispatcher
  reads arguments from the frame and writes the return value back into
  `tf->rax`; the epilogue restores the frame and `sysret`s.

The trapframe field order matches the push order exactly, so the same struct is
reused by the page-fault path and, later, by signals.

---

## 4. The system-call ABI (`kernel/syscall/syscall.c`)

Linux x86_64 numbers for the common calls, so a Linux-targeting libc can run
later with little change; MAKH-specific calls live above `0x200`.

| # | call | notes |
|---|------|-------|
| 1  | `write(fd, buf, n)` | stdout/stderr; copies via `copy_from_user` |
| 39 | `getpid()`          | current thread's pid |
| 60 | `exit(code)`        | leaves ring 3 for good |
| 0/3| `read`/`close`      | stubs (no VFS yet) |
| 0x200 | `getticks` (MAKH) | monotonic tick count |

Return convention is Linux's: `>= 0` success, `-errno` on failure.

---

## 5. Never trust a user pointer (`kernel/arch/uaccess.c`)

`copy_from_user` / `copy_to_user` run the byte copy in `__copy_user`. If the
user address faults, the page-fault handler looks up `uaccess_fixup(rip)` and,
on a match, resumes the copy at a cleanup label that returns the number of bytes
left — which the C wrapper turns into `-EFAULT`. The copy raises `EFLAGS.AC`
(via `popfq`, valid whether or not SMAP is enabled) so the kernel may touch user
pages under SMAP. A user pointer is first range-checked to the canonical lower
half. This is the Linux exception-fixup trick, in miniature.

---

## 6. A fault in ring 3 never takes down the kernel

The exception handler checks, in order:

1. **uaccess fixup** — a fault inside a user copy resumes at the fixup label.
2. **ring-3 fault** — if the faulting code was in ring 3 (`cs & 3 == 3`) during
   a user run, the program is killed: the handler `longjmp`s back to
   `run_user_program`, which reports `USER_FAULTED` and the vector. The kernel
   continues.
3. otherwise the usual panic / KFUZZ paths.

`run_user_program()` ties it together: map a user code page and stack, drop to
ring 3, and return when the program calls `exit()` or faults — reusing the
Phase 15 `setjmp`/`longjmp` as the unwind path.

---

## 7. Two real bugs this phase surfaced

**A dedicated trap stack.** `syscall`/IRQ/`#PF` from ring 3 must land on a kernel
stack that is *not* the live C stack of the thread hosting the run — otherwise
`TSS.rsp0` at the top would overwrite the `kernel_main → ktest →
run_user_program` frames we `longjmp` back into. Phase 16 allocates a separate
16 KiB trap stack. (In the real process model a user thread is *in* ring 3 with
its kernel stack empty when a trap arrives, so there this is just its kernel
stack.)

**The GS base vs. `swapgs`.** `syscall_entry` relies on `swapgs` loading the
per-CPU pointer from `IA32_KERNEL_GS_BASE`. But loading a segment *selector*
resets that segment's base to 0, and `context_switch.asm` reloads GS on every
switch — so between two user runs the per-CPU base is wiped and the second
run's `swapgs` yields a bad pointer (a textbook triple fault). Found by
bisection + QEMU reset logging. Fixed by pinning both GS-base MSRs immediately
before each ring-3 entry; the complete fix (reloading `KERNEL_GS_BASE` on every
kernel entry) arrives with SMP in Phase 22.

---

## 8. Deliberate limitations (handed to Phase 17+)

- **Non-preemptible user mode:** runs execute with interrupts masked. The
  programs are synchronous; preemptible user threads need the process model.
- **Single shared address space:** no per-process isolation yet. One program at
  a time, loaded at a fixed user virtual address.
- **No W^X / NX:** the leaf-flag mask drops bit 63 and `EFER.NXE` is off.
- **No ELF / `execve` / `fork`:** programs are raw machine code passed to
  `run_user_program`.

---

## 9. Tests (`kernel/tests/test_user.c`)

Three hand-assembled, position-independent ring-3 programs:

- **`write_then_exit`** — `write(1,"hi\n",3); exit(7)`; checks the exit code
  arrives through the ABI.
- **`ring3_fault_is_contained`** — writes to a null pointer (`#PF`), checks it is
  caught as `USER_FAULTED` vector 14, then runs another program to prove the
  kernel is fully alive.
- **`getpid_syscall_returns_value`** — `exit(getpid())`; checks a syscall return
  value round-trips through `sysret`.

85 in-kernel tests pass; `make stress` (12×) and `make smoke` (11/11) are clean.
