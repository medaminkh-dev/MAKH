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
| 20-D | Controlling terminal: blocking `read`, Ctrl+C, a `/bin/sh` shell | [PHASE20D_SHELL](PHASE20D_SHELL.md) |
| 20-E | `argv`/`envp`/`auxv`: the SysV initial stack; the shell parses arguments | [PHASE20E_ARGV](PHASE20E_ARGV.md) |
| 20-F | Job control: `setpgid`/`tcsetpgrp`, each command its own group, Ctrl+C hits the job | [PHASE20F_JOBCTL](PHASE20F_JOBCTL.md) |
| 20-G | User signal handlers: `sigaction`/`sigprocmask`/`sigreturn`, a signal frame on the user stack | [PHASE20G_SIGNALS](PHASE20G_SIGNALS.md) |
| 20-H | Thread-local storage: `arch_prctl(ARCH_SET_FS)`, per-process FS base | [PHASE20H_TLS](PHASE20H_TLS.md) |
| 20-I | Time & randomness: `clock_gettime`/`gettimeofday`/`nanosleep`/`getrandom` | [PHASE20I_TIME_RANDOM](PHASE20I_TIME_RANDOM.md) |
| 20-J | File metadata & listing: `stat`/`fstat`/`getdents64`/`fcntl` | [PHASE20J_STAT](PHASE20J_STAT.md) |
| 20-K | Pipes & redirection: `pipe`/`dup2`, fd inheritance, shell `a \| b` | [PHASE20K_PIPE](PHASE20K_PIPE.md) |
| 20-L | User threads: `clone`+`futex`+`set_tid_address`, shared VM/fds, join | [PHASE20L_THREADS](PHASE20L_THREADS.md) |
| 20-M | Wall clock: CMOS RTC → real `CLOCK_REALTIME`/`gettimeofday` epoch | [PHASE20M_RTC](PHASE20M_RTC.md) |
| 20-N | musl-readiness: `writev`/`readv`/`exit_group`/`madvise` (F20-a) | [PHASE20N_IOV](PHASE20N_IOV.md) |
| 20-O | A real C program on **musl** libc: static-PIE loader, auxv, the TLS-base fix (F20) | [PHASE20O_MUSL](PHASE20O_MUSL.md) |
| 20-O2 | **busybox `sh`** runs: real busybox static-PIE, `getppid`/uid family, spawn-with-argv (F20-c) | [PHASE20O2_BUSYBOX](PHASE20O2_BUSYBOX.md) |
| 20-P | **Persistent storage**: a polled legacy **virtio-blk** disk, read/write sectors (G2-a) | [PHASE20P_VIRTIO_BLK](PHASE20P_VIRTIO_BLK.md) |
| 20-Q | **ext2 filesystem** (read-only): mount an on-disk ext2 from virtio-blk into the VFS (G2-b) | [PHASE20Q_EXT2](PHASE20Q_EXT2.md) |
| 20-R | **ext2 write path**: create files + write (block/inode alloc, dir entries) (G2-c) | [PHASE20R_EXT2_WRITE](PHASE20R_EXT2_WRITE.md) |
| 20-S | **Toolchain syscalls**: `execve` envp + the `openat`/`*at` family; musl file I/O on ext2 (G3) | [PHASE20S_G3_SYSCALLS](PHASE20S_G3_SYSCALLS.md) |
| 20-T | **ext2 directory ops**: `mkdir`/`unlink`/`rmdir`/`rename`/`truncate` (G2-d) | [PHASE20T_EXT2_DIROPS](PHASE20T_EXT2_DIROPS.md) |
| 21 | **Higher-half kernel**: kernel → top -2 GiB, HHDM, the low half freed per-process, standard non-PIE binaries at 0x400000 (F21 Path A) | [PHASE21_HIGHERHALF](PHASE21_HIGHERHALF.md) |
| 21-A | **tcc self-hosts**: a real C compiler runs on MAKH, compiles C to a native executable, and MAKH runs it (F21) | [PHASE21A_SELFHOST_TCC](PHASE21A_SELFHOST_TCC.md) |
| 21-B | **libc compile on MAKH**: tcc links a real `stdio`/`stdlib`/`string` program against a staged musl sysroot, on MAKH, and MAKH runs it (F21) | [PHASE21B_LIBC_COMPILE](PHASE21B_LIBC_COMPILE.md) |
| 21-C | **make on MAKH**: GNU make drives tcc across a multi-file project (fork/exec/wait), on MAKH, and MAKH runs the result (F21) | [PHASE21C_MAKE](PHASE21C_MAKE.md) |
| 21-D | **self-host summit**: MAKH rebuilds a program it ships (`/bin/muslhello`) from source with make+tcc, and the rebuilt binary behaves identically (F21) | [PHASE21D_SELFHOST](PHASE21D_SELFHOST.md) |

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
20-D controlling tty . blocking read + Ctrl+C + /bin/sh  ─▶ an interactive prompt
                       (the "run a command" sequence)      (boot: makh.sh)
20-E argv/envp/auxv .. SysV initial stack; sh parses args ─▶ "echo a b" works
                       (the ABI a C runtime stands on)        (musl-ready stack)
20-F job control ..... per-command pgroup + tcsetpgrp     ─▶ Ctrl+C hits the job
                       (the shell hands over the terminal)    (not the shell)
20-G signal handlers . sigaction + signal frame + sigreturn ─▶ catch & resume
                       (a process catches its own signals)    (gap #1 complete)

past gap #1 — toward a libc userland (Stage 1: the syscall surface):
20-H TLS base ........ arch_prctl(SET_FS) + per-switch repin ─▶ %fs:0 works
                       (the thread pointer musl needs first)    (musl enabler)
20-I time + random ... clock_gettime/nanosleep/getrandom     ─▶ libc startup bits
                       (interruptible sleep; xoshiro256** RNG)  (1.1 of Stage 1)
20-J stat + listing .. stat/fstat/getdents64/fcntl           ─▶ ls/find/opendir
                       (byte-exact struct stat for musl)        (1.2 of Stage 1)
20-K pipe + dup2 ..... fd refcounts, fork-inherit, dup2      ─▶ echo abc | countin
                       (fds 0/1/2 table-first, console fallbk)  (1.3 of Stage 1)
20-L clone + futex ... shared-VM threads, futex, CLEARTID join ─▶ pthreads base
                       (aspace/fd refcounted lifetime)          (1.4 of Stage 1)
20-M wall clock ...... CMOS RTC epoch anchors CLOCK_REALTIME  ─▶ real dates/time()
                       (read once at boot, advance monotonic)   (1.5 — Stage 1 done)

F20 — the musl port:
20-N iov + exit_group  writev/readv/exit_group/madvise        ─▶ libc stdio path
                       (crt0 + buffered-stdio gaps)             (F20-a)
20-O a real C program  static-PIE loader + full auxv +        ─▶ musl hello runs
                       the lost-FS-base-on-switch fix           (F20 — done)
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
