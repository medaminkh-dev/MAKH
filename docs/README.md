# MakhOS design docs — the map

Each phase of MakhOS adds one brick to the kernel, with a design document that
explains *what* it does, *how*, what bugs it surfaced, and what it deliberately
left for later. This index threads them in order so the jumps between phase
numbers are navigable rather than mysterious.

> Phase **numbers are historical** — they are the order the work happened, not
> a contiguous range. Early boot phases (1–4) and a few consolidation phases
> live in code and in the reports below rather than in their own design doc;
> everything with a design doc is linked here.

## The journey

| Phase | Brick | Doc |
|---|---|---|
| 1–4 | Boot (Multiboot2/GRUB), VGA + serial, physical memory, paging | *(in code; see the v0.0.2 report)* |
| 5 | PIC remap + PIT timer | [PHASE5_PIC_TIMER](PHASE5_PIC_TIMER.md) |
| 6 | PS/2 keyboard + line editing | [PHASE6_KEYBOARD](PHASE6_KEYBOARD.md) |
| 7 | GDT + TSS | [PHASE7_GDT_TSS](PHASE7_GDT_TSS.md) |
| 8 | `syscall`/`sysret` MSR plumbing | [PHASE8_SYSCALL](PHASE8_SYSCALL.md) |
| 9 | Processes & scheduling (v1) | [PHASE9_PROC](PHASE9_PROC.md) · [complete](PHASE9_PROC_COMPLETE_DOCUMENTATION.md) |
| 10–13 | Test harness, preemptive scheduler v2, sleep/wait queues, POSIX threads | *(see [VERIFICATION_REPORT](VERIFICATION_REPORT.md))* |
| 14 | Networking: PCI, e1000, TCP/IP, BSD sockets, shell | [PHASE14_NETWORK](PHASE14_NETWORK.md) |
| 15 | KFUZZ: a ring-0, coverage-guided self-fuzzer | [PHASE15_KFUZZ](PHASE15_KFUZZ.md) |
| 16 | Ring-3 userspace: syscall/sysret ABI, uaccess, fault containment | [PHASE16_USERSPACE](PHASE16_USERSPACE.md) |
| 17 | VM v2: per-process address spaces, copy-on-write, NX/W^X | [PHASE17_VMV2](PHASE17_VMV2.md) |
| 18 | Filesystem: VFS, tmpfs, devfs, tar initrd, fd/syscalls | [PHASE18_VFS](PHASE18_VFS.md) |
| 19 | Signals, process groups, TTY line discipline | [PHASE19_SIGNALS_TTY](PHASE19_SIGNALS_TTY.md) |
| 20-A | The user process model: ELF load, preemptible ring 3, `waitpid` | [PHASE20A_PROCESS_MODEL](PHASE20A_PROCESS_MODEL.md) |
| 20-B | Anonymous memory: `brk`, `mmap`/`munmap`/`mprotect` | [PHASE20B_MMAP_BRK](PHASE20B_MMAP_BRK.md) |
| 20-C | Working directory: per-process cwd, `chdir`/`getcwd`, relative paths | [PHASE20C_CWD](PHASE20C_CWD.md) |
| 20-A-2 | `fork` (copy-on-write) + `execve` (replace image) + `wait4` | [PHASE20A2_FORK_EXECVE](PHASE20A2_FORK_EXECVE.md) |

Reports that span phases: [CHANGELOG_v0.0.2](CHANGELOG_v0.0.2.md),
[GIT_DIFF_REPORT_v0.0.2](GIT_DIFF_REPORT_v0.0.2.md),
[VERIFICATION_REPORT](VERIFICATION_REPORT.md).

## How the recent phases connect

Phases 16–20-A are one arc — building a real userland from the ground up — and
each doc ends with an explicit **"Deferred to Phase N"** table, so the hand-off
is always visible:

```
16  ring-3 ABI ....... "one program, non-preemptive"      ─┐
17  address spaces ... "COW exists, nothing schedules two" ─┤
18  VFS / initrd ..... "files to load programs from"        ├─▶ 20-A fuses these
19  signals / tty .... "SIGCHLD/waitpid need a process"    ─┘    into a real,
                                                                 scheduled,
20-A user process .... fork/execve/cwd/tty-read ─▶ 20-A-2        preemptible
20-B anon memory ..... brk + mmap/munmap/mprotect              user process
                       (what malloc stands on)
20-C working dir ..... cwd + chdir/getcwd + relative paths     (shell-ready
20-A-2 fork/execve ... COW clone + replace-image + wait4        pieces)
                       (the "run a command" sequence)
```

## Doc conventions (phases 16+)

Recent docs share one shape, so they read consistently:

1. **Scope** — what the brick is, and what it explicitly defers.
2. Numbered sections per component, named by source file.
3. **Fuzzed** — the KFUZZ target(s) that hammer the new surface, and what they found.
4. **Tests** — the in-kernel `KTEST`s and the headline numbers.
5. **Deferred to Phase N** — a table handing work to the next brick.

Older docs (phases 5–9) predate this template and use a longer
release-note style; their content stands, and this index is the thread that
ties the whole sequence together.
