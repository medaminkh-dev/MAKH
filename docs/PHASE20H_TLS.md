# MakhOS Phase 20-H: `arch_prctl(ARCH_SET_FS)` — thread-local storage base

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a ring-3 process can set its FS base, so `%fs`-relative
thread-local storage works; the base is per-process and survives preemption.
147 in-kernel tests
**Depends on:** Phase 20-A (preemptible user processes, `arch_prepare_switch`),
Phase 16 (the GS/per-CPU MSR plumbing this mirrors)

---

## 1. Scope

This is the first brick past the process model (gap #1), and the single most
important syscall for running an unmodified libc: **`arch_prctl(ARCH_SET_FS)`**.
A C runtime (musl, glibc) puts its thread-control block behind the `FS` segment
and reaches thread-local variables as `%fs:offset`; nothing starts without a
working FS base.

- **`arch_prctl(ARCH_SET_FS, addr)`** — point this process's FS base at `addr`;
- **`arch_prctl(ARCH_GET_FS, &addr)`** — read it back.

`ARCH_SET_GS`/`ARCH_GET_GS` are deliberately refused (`-EINVAL`): `GS` is the
kernel's (the per-CPU block reached via `swapgs`), so it is not the user's to
set.

**Deferred:** the FSGSBASE instructions (`wrfsbase`) as a fast path — we use the
`IA32_FS_BASE` MSR, which needs no CPU feature bit; a full `set_thread_area`/GDT
TLS entry (32-bit style) is not needed on x86-64.

---

## 2. Per-process FS base (`syscall.c`, `arch/usermode.c`, `proc.h`)

The PCB gains one field, `fs_base`, zero until set. The syscall writes it two
ways at once:

```
arch_prctl(ARCH_SET_FS, addr):
    cur->fs_base = addr;          // remembered for later switches
    wrmsr(IA32_FS_BASE, addr);    // effective for the rest of this timeslice
```

The subtlety is preemption. `context_switch` saves and restores general
registers but **not** MSRs, so the FS base would be lost the first time the
process is scheduled off. The fix mirrors exactly what Phase 16/20-A already do
for the TSS stack and the GS base: `arch_prepare_switch`, which runs on every
context switch, re-pins the incoming process's FS base from its PCB:

```
arch_prepare_switch(next):
    ... TSS.rsp0, GS base (unchanged) ...
    wrmsr(IA32_FS_BASE, next->fs_base);   // 0 for a kernel thread
```

Because the kernel itself never reads `FS`, this is the *only* place FS needs
tending — there is no `swapfs`, no kernel-side FS user. A kernel thread simply
carries `fs_base == 0`, so the same unconditional `wrmsr` restores it cleanly.
`fork` inherits the parent's `fs_base` (the PCB is copied); `execve` leaves it
as the fresh image's crt0 will set its own.

## 3. Fuzzed

No new KFUZZ target: `arch_prctl` is a two-case switch over a single MSR and a
bounded `copy_to_user` for the get, with no parsing of untrusted buffers. The
adversarial surface is a bad pointer to `ARCH_GET_FS` (returns `-EFAULT`) and an
unknown code (`-EINVAL`); both are on the acceptance path.

## 4. Tests (`kernel/tests/test_tls.c`) + program (`user/tlstest.c`)

- **`tls.arch_prctl_set_fs_round_trip`** (`/bin/tlstest`) — the program points
  FS at a one-word block holding `55`, reads it back through `%fs:0` (exactly
  what a TLS access compiles to), confirms `ARCH_GET_FS` reports the same base,
  and exits **55**. The status proves the base took effect for a real `%fs`
  access *and* round-trips through get — and, because the read happens after a
  syscall boundary where preemption can intervene, that it survives a context
  switch.

**147 in-kernel tests pass**; `make stress` (serial) is clean.

## 5. Deferred

| Item | Why it waits |
|---|---|
| `wrfsbase` fast path | needs the FSGSBASE CPU feature + CR4 bit; the MSR works everywhere |
| `set_thread_area` (GDT TLS) | the 32-bit TLS ABI; x86-64 uses FS base directly |
| full musl TCB bring-up | needs `pipe`/`dup`, `stat`/`getdents`, and a dynamic or static libc |

With a working FS base, the thread-pointer foundation a libc needs is in place.
The remaining userland gaps are the wider syscall surface
(`pipe`/`dup2`/`stat`/`fstat`/`getdents64`) and persistent storage
(virtio-blk + a real filesystem).
