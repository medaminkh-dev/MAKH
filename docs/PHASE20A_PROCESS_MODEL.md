# MakhOS Phase 20-A: the user process model

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE (first brick) — load an ELF from the initrd, run it in
**ring 3 preemptively** inside its **own address space**, and reap it with
`waitpid`; 118 in-kernel tests, fuzzed (incl. a new `elf` target), stress clean
**Depends on:** Phase 16 (ring-3 ABI), Phase 17 (address spaces), Phase 18
(VFS/initrd), Phase 19 (signals)

---

## 1. Scope

Phases 16–19 each left the same hole: there was no **persistent, preemptible
user process with its own address space, scheduled by the kernel**. Phase 16
ran one program non-preemptively through `run_user_program`; Phase 17 built
address spaces and COW but nothing scheduled two of them; Phase 19 delivered
signals to kernel threads cooperatively. This phase is the **keystone that
fuses 16 + 17 + 19 into a real process**:

```
proc_spawn_user("/bin/hello")
   └─ vfs_resolve → elf_load into a fresh address_space_t (private CR3)
   └─ map a user stack, create a kernel thread, mark it is_user
   └─ the thread is scheduled like any other, but context_switch loads its
      CR3 and it runs in ring 3 with interrupts on (the timer preempts it)
   └─ SYS_EXIT → zombie → wake the parent's child_wq + SIGCHLD
parent: sys_waitpid(pid) → find the zombie child → reap → exit status
```

What this brick **does**: ELF64 load into a per-process address space;
preemptible ring-3 execution; `exit`; `waitpid` (`pid>0` and `-1`); `SIGCHLD`
to the parent; multiple live processes with distinct address spaces.

**Deferred to Phase 20-A-2** (§10): `fork`, `execve`-replacing-self,
per-process cwd + relative paths, the keyboard→tty→blocking-`read` path, an
`init`(PID 1) that spawns a shell, and the full SysV auxiliary vector
(`AT_PHDR`/`AT_ENTRY`/`AT_RANDOM`/`AT_PAGESZ`) that musl needs. This brick sets
`argc = 0`; nothing yet depends on argv/envp/auxv.

---

## 2. The process = a scheduled thread + an address space

Phase 20-A adds a handful of fields to the PCB (`kernel/include/proc.h`):

- `uint8_t is_user` — this thread runs code in ring 3;
- `void* aspace` — its `address_space_t` (Phase 17), or `NULL` for a kernel thread;
- `uint64_t user_entry`, `user_stack` — where ring 3 starts;
- `wait_queue_t child_wq` — a parent blocks here in `waitpid`.

A user process is therefore an ordinary kernel thread that *additionally* owns
an address space and drops to ring 3. The scheduler is unchanged in shape; the
only new work happens at the switch boundary (§3).

## 3. The per-switch entry layer (`kernel/arch/usermode.c`)

Preemptible ring 3 means a timer IRQ can arrive at any instruction of a user
program, and a switch can land on a *different* process. Three things must be
true for the incoming thread **before** `context_switch` runs; `schedule()`
calls `arch_prepare_switch(next)` to establish them:

1. **`TSS.rsp0` = this thread's kernel stack top.** A ring-3 → ring-0 trap
   (syscall-less IRQ, or #PF) switches to `TSS.rsp0`; it must be the trapping
   thread's own kernel stack, set on every switch, or two processes would share
   a trap stack and corrupt each other.
2. **`percpu.kernel_rsp` = the same top**, because the `syscall` fast path
   (`swapgs; mov rsp,[gs:0]`) takes its kernel stack from the per-CPU block.
3. **The GS base is re-pinned to the per-CPU block.** `context_switch` reloads
   the GS *selector* (`mov gs, …`), which resets the active GS base; re-pinning
   both `IA32_GS_BASE` and `IA32_KERNEL_GS_BASE` to `&boot_cpu` on every switch
   keeps `swapgs` correct no matter what. (This generalises the Phase-16 GS-base
   hazard into an always-true single-CPU invariant.)

The CR3 itself travels **inside** `context_switch`: each PCB's saved context
carries `cr3`, and a user thread's is set to its space's PML4
(`t->context.cr3 = as->pml4_phys`). Kernel threads keep the kernel PML4, so
switching kernel↔user↔kernel just loads the right CR3 each time.

`enter_user_mode` pushes an `iretq` frame with `RFLAGS = 0x202` (**IF = 1**), so
the user program is interruptible from the first instruction — that is what
makes ring 3 preemptible at all.

## 4. The ELF64 loader (`kernel/proc/elf.c`)

`elf_load(vnode, address_space, &entry)` reads a **static** ELF64 `ET_EXEC`
through the VFS and maps each `PT_LOAD` segment into the target space:

- validate magic, `ELFCLASS64`, `ET_EXEC`, and a sane program-header table;
- for each `PT_LOAD`, allocate zeroed frames, copy the file slice into each
  page through the identity map (so the loader never needs the space active),
  set the frame refcount to 1 so `vmspace_destroy` reclaims it, then
- **re-map with W^X**: an executable segment becomes read-only + executable; a
  writable segment is RW + NX; read-only data is NX.

**Hardening the loader is a security boundary, not a nicety** — `elf_load`
parses entirely untrusted bytes. The parser enforces, with overflow-safe
arithmetic, that every segment lies wholly inside the **private per-process
window** (one PML4 slot, `[0x2000_0000_0000, 0x2080_0000_0000)`); otherwise
`vmspace_map` would walk into the *shared kernel* PML4 entries and corrupt
them. It also caps a single segment at **16 MiB** (`USER_SEG_MAX`) and rejects
`p_filesz > p_memsz`. See §8 for why those two limits exist.

## 5. Spawning (`kernel/proc/user.c`)

`proc_spawn_user(path)`:

1. `vfs_resolve(path)` → `-ENOENT` if missing;
2. `vmspace_create` a fresh space; `elf_load` into it (errors unwind and
   destroy the space — no leak on any failure path);
3. map a 64 KiB user stack (RW + NX) near the top of the window and write
   `argc = 0`;
4. `thread_create(user_trampoline)`, then set `is_user`, `aspace`,
   `user_entry`, `user_stack`, and `context.cr3`.

The trampoline runs once in ring 0 (CR3 already the user space) and calls
`enter_user_mode(entry, stack)`, which never returns. A fault taken while the
process runs in ring 3 is routed by the IDT to `proc_user_fault`, which
terminates the process with `128 + SIGSEGV` (139) — the kernel is never at
risk (`kernel/arch/idt.c`).

## 6. Exit, `SIGCHLD`, and `waitpid`

`SYS_EXIT` from a user process calls `thread_exit(code & 0xff)`. The exiting
thread becomes a `ZOMBIE`, wakes its parent's `child_wq`, and raises `SIGCHLD`
on the parent.

`sys_waitpid(pid, &status)` scans for a matching **user-process** child; a
zombie is reaped (`proc_reap` frees the address space, kernel stack, pid and
table slot) and its code returned; if children exist but none has exited, the
caller blocks on `child_wq`; with no children it returns `-ECHILD`. `pid > 0`
waits for one child; `pid == -1` waits for any.

## 7. What internal reverse-engineering found

Two bugs were found by testing the model against itself rather than by reading
the code, which is the point of the exercise:

- **`waitpid` could hang forever.** The scan originally counted *every* child
  of the caller. PID 1 also parents kernel worker threads (`netd`, a pthread
  pool) that are permanently `BLOCKED` and never exit, so `have_children` was
  always true and `waitpid(-1)` never returned `-ECHILD`. Fix: `waitpid` tracks
  only `is_user` children; kernel threads are reaped through `thread_join`.
- **`SIGCHLD` piled up on the parent.** A child's exit raised `SIGCHLD`, but a
  signal whose *effective disposition is ignore* (explicit `SIG_IGN`, or the
  `SIG_DFL`-is-ignore signals like `SIGCHLD`) must be **discarded on
  generation**, not made pending. Without that, a parent that never installs a
  `SIGCHLD` handler accumulated pending signals and could take spurious EINTRs.
  Fixed in `signal_send`/`signal_send_pgrp` via one `effectively_ignored()`
  helper — which is also plain POSIX-correct.

## 8. Fuzzed (`elf` target)

A new KFUZZ **`elf`** target drives the loader adversarially: it writes a
mostly-valid static ELF64 to a scratch file, then corrupts the high-value
fields (`e_phnum`, `p_vaddr`, `p_offset`, `p_filesz`, `p_memsz`) and a slice of
pure garbage, loads it into a throwaway space, and destroys it. The oracle is
strict per-iteration **page conservation** (create → load → destroy must be
net-zero), so a leaked frame or a double free on any error path is caught at
once; the ring-0 sandbox catches any fault as a finding.

This target immediately earned its place. On seed `0xd58a…81a9` the watchdog
caught `elf_load` **hung**: a large-but-in-window `p_memsz` drove a
multi-million-page allocation loop — a denial of service reachable from a
single malformed header. That is the origin of the `USER_SEG_MAX` cap (§4). A
**40,000-iteration** campaign is now clean (no crashes, no leaks).

## 9. Tests (`kernel/tests/test_proc.c`) + user programs (`user/`)

Four tiny freestanding programs (`-mcmodel=large`, linked at the window base
`0x2000_0040_0000`) are built into the initrd under `/bin`:

| Program | Does | Exit |
|---|---|---|
| `hello` | `write(1, "hi from user\n")` | 42 |
| `getpid` | `getpid()` | `pid & 0x7f` |
| `spin` | a long ring-3 loop | 9 |
| `faulter` | writes to `0x1234` (unmapped) | killed by SIGSEGV → 139 |

Tests:

- **`spawn_elf_and_wait`** — `hello` runs in ring 3 and `waitpid` returns 42.
- **`exit_code_from_getpid`** — a process exits with its own pid.
- **`two_processes_distinct_address_spaces`** — two live processes with
  distinct CR3s; both reaped with `waitpid(-1)`; the third `waitpid` is `-ECHILD`.
- **`ring3_is_preemptible`** — while `spin` loops in ring 3, a `sleep` in the
  parent still wakes (the timer is preempting ring 3).
- **`user_fault_becomes_sigsegv`** — `faulter` dies with status 139, kernel unharmed.
- **`spawn_wait_many_no_leak`** — 40 spawn/wait cycles leave free memory exactly
  where it started (no address-space / stack / pid leak).
- **`spawn_missing_path_is_enoent`** — a missing path is a clean `-ENOENT`.

**118 in-kernel tests pass**; `make stress` is clean; the `elf` fuzz target and
the all-targets campaign are clean.

## 10. Deferred to Phase 20-A-2

| Item | Why it waits |
|---|---|
| `fork()` (COW the user slot) | the plumbing (Phase 17 `vmspace_fork`) exists; needs the syscall + trapframe copy |
| `execve()` replacing the caller's own image | needs to swap CR3 and tear down the old space under the running thread |
| per-process cwd + relative paths (`chdir`/`getcwd`) | the VFS resolve is absolute-only today |
| keyboard → tty → blocking `read()` | wire the keyboard IRQ into the Phase-19 line discipline and block a user `read` on it |
| `init` (PID 1) that `execve`s a shell | needs `fork`+`execve`+ the tty read path above |
| full auxv (`AT_PHDR`/`AT_ENTRY`/`AT_RANDOM`/`AT_PAGESZ`) + argv/envp | required by musl; this brick passes `argc = 0` |

These, plus `mmap`/`brk` (deferred from Phase 17) and the wider syscall surface,
are the remaining prerequisites before a musl/busybox userland (Phase 20/F20).
