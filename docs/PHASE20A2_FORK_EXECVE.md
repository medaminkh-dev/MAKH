# MakhOS Phase 20-A-2: fork & execve

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `fork()` (copy-on-write clone), `execve()` (replace the
image in place), and `wait4()`; 136 in-kernel tests, stress clean
**Depends on:** Phase 17 (COW address spaces), Phase 20-A (process model),
Phase 20-B/C (the image is built the same way; cwd is inherited)

---

## 1. Scope

The two system calls a shell is built on:

- **`fork()`** — duplicate the caller into a new process whose address space is
  a copy-on-write clone of the parent's. The child sees `0`, the parent sees
  the child's pid.
- **`execve(path, …)`** — replace the caller's image with a freshly loaded
  program, keeping its pid, cwd and open descriptors.
- **`wait4()`** — the syscall form of the Phase 20-A `waitpid`, so a ring-3
  parent can reap its children.

With these, `fork()` + `execve()` + `wait4()` is the classic "run a command"
sequence, demonstrated end to end by `/bin/forkexec`.

**Deferred:** `argv`/`envp`/`auxv` (execve starts the new image with `argc = 0`
for now — musl needs the full vector, which lands with the libc work), open-fd
inheritance across `fork` (the console fast path means a child still has
stdin/out/err; real fd inheritance arrives with the file-refcount work next to
`pipe`/`dup`), and copy-on-write of the kernel-side during `vfork`.

---

## 2. fork() (`proc/user.c`)

`fork` is driven from `syscall_dispatch`, which hands it the live trapframe:

1. **Address space:** `vmspace_fork()` (Phase 17) clones the parent's user
   region copy-on-write — the page tables are duplicated but the data frames
   are shared read-only with their refcounts bumped, so neither side's writes
   are seen by the other.
2. **PCB + kernel stack:** a fresh process, mirroring `thread_create`'s
   allocation, inheriting the parent's cwd, brk/mmap cursors, process group,
   session and signal masks.
3. **The return frame — the subtle part.** The child must resume *in ring 3 at
   the same instruction as the parent, with `rax = 0`*. We copy the parent's
   trapframe to the top of the child's kernel stack, set its `rax` to 0, and
   point the child's saved context at **`fork_child_entry`** with its stack
   pointer at that copy. When the scheduler first runs the child,
   `context_switch` iretq's into `fork_child_entry`, which restores the general
   registers and **IRETQ**s to ring 3 using the trapframe's `rip/cs/rflags/
   rsp/ss` tail.

   Crucially this uses **IRETQ, not SYSRET**, and does **no `swapgs`** — exactly
   like `enter_user_mode`. A normal syscall return pairs two `swapgs` (entry +
   exit); a child reaching only the exit half would run an unpaired one. IRETQ
   sidesteps that and preserves every register exactly, which `fork` requires.

The child is made runnable last (`sched_admit`), so it can never be scheduled
half-formed.

## 3. execve() (`proc/user.c`)

`execve` builds the new image in a brand-new address space **first**
(`build_user_image`, shared with spawn), so a failed exec (bad path, not an
ELF) leaves the caller untouched and returns `-errno`. Once the image is ready
it swaps atomically: install the new space, load its CR3, free the old space
(safe — we run on the shared kernel stack, whose mapping both spaces carry),
reset the brk/mmap cursors, and **rewrite the trapframe** so the ordinary
syscall-return path lands in the new program with a clean register file. The
pid, cwd and fd table carry over; the heap and mappings are fresh.

## 4. The GS-base invariant (`context_switch.asm`, `arch/usermode.c`)

Bringing up `fork` surfaced a latent bug that predates it. `context_switch`
reloaded the `gs` **selector** on every switch, which resets the active GS base
to 0. For a thread resuming a **blocking** syscall (e.g. a parent in
`wait4`), the unwind to `syscall_return`'s `swapgs` then found the two GS bases
the wrong way round and returned to ring 3 with `KERNEL_GS_BASE = 0` — so the
process's *next* syscall read its kernel stack from address 0 and triple
faulted. No earlier phase hit this because no user process had yet made a
blocking syscall and then another.

The fix is the "full" one the Phase 16 notes anticipated: the GS base is owned
entirely by `arch_prepare_switch` (which pins both bases to the per-CPU block
on every switch) and by `swapgs`; `context_switch` no longer touches the `gs`
selector. The invariant "both GS bases are the per-CPU block" now holds across
every switch, which makes `swapgs` idempotent and the whole class of
blocking-syscall GS bugs impossible.

## 5. Copy-on-write faults (`arch/idt.c`)

A write to a COW page (a forked child or parent touching shared memory) is now
**resolved, not fatal**: the page-fault handler calls `vmspace_cow_fault()` to
hand the writer a private copy and retries the instruction. It runs before the
uaccess fixup, so it also covers a kernel `copy_to_user` into a COW page (e.g.
`wait4` writing the status word to a parent's not-yet-copied stack).

## 6. Tests (`kernel/tests/test_fork.c`) + programs (`user/`)

- **`parent_waits_for_child`** (`/bin/forktest`) — child exits 7, parent
  `wait4`s and sees the code.
- **`cow_isolates_child_writes`** (`/bin/forkcow`) — the child mutates a `.data`
  global; the parent's copy is unchanged, proving COW isolation.
- **`fork_then_execve_runs_new_image`** (`/bin/forkexec`) — the child
  `execve`s `/bin/hello` (exit 42); the parent reaps it.
- **`fork_exec_many_no_leak`** — 20 fork+exec+wait cycles leave free memory
  exactly where it started (no address-space, stack or pid leak).

The user programs link with a two-segment layout (`user/user.ld`: R-X text,
RW data), so `.data` is genuinely writable and copy-on-write across `fork`.

**136 in-kernel tests pass**; `make stress` is clean across repeated reboots.

## 7. Deferred

| Item | Why it waits |
|---|---|
| `argv`/`envp`/`auxv` at `execve` | needs the full SysV initial stack; required by musl |
| open-fd inheritance across `fork` | needs file-object refcounts (arriving with `pipe`/`dup`) |
| close-on-exec, `execve` signal reset to default | needs per-fd flags / handler table |
| threads sharing an address space (`clone`) | a different sharing model than `fork`'s COW |

`fork` + `execve` + `wait4` are the last structural piece of the process model.
What remains before an interactive shell is the keyboard→tty blocking-`read`
path, an `init` that execs a shell, and the `pipe`/`dup`/`stat`/`getdents`
surface.
