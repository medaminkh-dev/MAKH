# MakhOS Phase 20-O2 (F20-c): busybox `sh` runs on MAKH

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a real, unmodified **busybox** (1.37.0), built from source
as a static-PIE against musl, runs its `ash` shell on MAKH: it parses a script,
does arithmetic, writes to stdout, and exits with a computed status.
**159 in-kernel tests.**
**Depends on:** 20-O (static-PIE loader + musl), 20-A-2 (`execve`), 20-G (signals)

---

## 1. Scope

With a real libc proven (20-O), this brick runs a real **program** people use:
busybox's `ash` shell. The point is not busybox itself but that MAKH's syscall
ABI is faithful enough that a large, third-party, unmodified C program just
works.

- **Built from source**, not vendored: busybox 1.37.0 (`busybox.net`,
  sha256 `3311dff3…43a4`), configured down to the shell plus a few coreutils,
  compiled static-PIE with the same ziglang/musl toolchain as 20-O. The binary
  is checked in at `user/musl/busybox` and ships into the initrd as
  `/bin/busybox`; §5 is the exact, repeatable recipe.
- **Minimal config on purpose.** `allnoconfig` + `ash` (`sh`) + `echo`/`cat`/
  `pwd`/`sleep`/`true`/`false`. A minimal shell keeps both the compile surface
  and the kernel syscall surface small for this first run; more applets are a
  later brick.

---

## 2. Building static-PIE through a wrapper (`user/musl` toolchain)

busybox makes `CONFIG_STATIC` and `CONFIG_PIE` mutually exclusive, but MAKH's
loader needs **both** (static *and* PIE — a self-relocating image at the high
user base, 20-O). So both config options are left **off** and a small `zig cc`
wrapper drives it instead:

- every target object is compiled `-fPIE`, and every link gets `-pie -static`
  (zig rejects the combined `-static-pie` spelling);
- the wrapper drops three GNU-ld-only arguments busybox's `trylink` passes that
  LLD's linker does not accept: `--warn-common`, `-Map,<file>`, `--verbose`
  (with static musl the library probe needs none of them).

The result is an `ET_DYN` static-PIE, byte-for-byte the kind of binary 20-O's
loader already runs.

## 3. New syscalls (`syscall/syscall.c`)

ash's startup needed five trivial identity calls MAKH did not have yet:

- **`getppid`** (110) — returns the PCB's `parent_pid`.
- **`getuid`/`geteuid`/`getgid`/`getegid`** (102/107/104/108) — MAKH is a
  single-user system, so every id is **root (0)**. A shell reads these only to
  pick the prompt character and a few permission shortcuts.

Everything else ash touches already existed: `arch_prctl`, `set_tid_address`,
`sched_getaffinity` (20-O), `mmap`/`munmap` (20-B), `rt_sigaction`/
`rt_sigprocmask` (20-G), `stat` (20-J), `write`, `execve` (20-A-2), `exit_group`
(20-N).

## 4. `proc_spawn_user_argv` (`proc/user.c`)

The kernel could previously launch a program only with `argv == { path }`.
`proc_spawn_user_argv(path, argv)` lets it pass a full argument vector, so it can
start `busybox sh -c '<script>'` the way a real `init` would. `proc_spawn_user`
is now a one-line wrapper over it; the SysV stack builder is unchanged.

## 5. The exact build recipe (reproducible)

```sh
pip install ziglang                       # vendors musl + a cross cc
curl -O https://busybox.net/downloads/busybox-1.37.0.tar.bz2
tar xjf busybox-1.37.0.tar.bz2 && cd busybox-1.37.0
make allnoconfig
# enable: STATIC=n PIE=n (driven by the wrapper), SHELL_ASH, ASH, SH_IS_ASH,
#         ASH_ECHO/TEST/PRINTF, ECHO, CAT, PWD, SLEEP, TRUE, FALSE, UNAME,
#         FEATURE_SH_MATH   (then: yes "" | make oldconfig)
make CC=<zcc-wrapper> HOSTCC=gcc          # zcc = §2 wrapper around `zig cc`
# -> ./busybox : ELF static-pie, ~140 KiB; copied to user/musl/busybox
```

The wrapper (kept with the build, not in-tree) is:

```sh
#!/bin/bash
args=(); compile=0
for a in "$@"; do case "$a" in
  -c) compile=1 ;;
  -Wl,--warn-common|-Wl,--verbose) continue ;;
  -Wl,-Map,*) continue ;;
esac; args+=("$a"); done
extra=(-fPIE); [ "$compile" -eq 0 ] && extra+=(-pie -static)
exec python3 -m ziglang cc -target x86_64-linux-musl "${extra[@]}" "${args[@]}"
```

## 6. Fuzzed

No new KFUZZ target: the new syscalls are constant-returning identity calls, and
the ELF path busybox exercises is the static-PIE loader already hammered by
`kfuzz.target_elf` (20-O). busybox itself is the adversarial input here — a
140 KiB third-party program driven end to end through the syscall ABI.

## 7. Tests (`kernel/tests/test_busybox.c`)

- **`busybox.ash_runs_a_script`** — spawns `/bin/busybox sh -c 'echo bb-$((6*7));
  exit $((6*7))'` and expects status **42**. That proves ash tokenised the line,
  evaluated the arithmetic, ran `echo` (its output appears on the console), and
  exited with the computed value — the whole shell path on MAKH's ABI.

**159 in-kernel tests pass**; `make stress` (serial) is clean.

## 8. Deferred to F20-d / later

- More applets run as **external** commands (`ls`, `cat foo | wc`), which fork +
  `execve` busybox again — exercises the pipeline + fork path under a real shell.
- Interactive ash over the tty (job control, line editing) rather than `-c`.
- A fuller config (closer to `defconfig`) once persistent storage (G2) gives it
  a real filesystem to work on.
