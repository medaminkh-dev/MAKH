# MakhOS Phase 26: fast quiet boot + a usable default shell

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a normal boot is short and lands straight on the
user-space shell, which now finds commands by name and ships native `ls`, `cat`
and `pwd`. **183 in-kernel tests.**
**Depends on:** 20-D (controlling terminal + shell), U1-b (/bin + busybox),
25/FB-1 (framebuffer console)

---

## 1. Why

Two rough edges on a real (non-test) boot:

1. **The boot was long and dry** — `kernel_main` ran the full brick-by-brick
   self-test parade every time (VMM/heap/GDT/timer/syscall smoke tests, a 2-second
   timer tick-count check, a context-switch demo, and a keyboard test that
   *blocked* until you typed `TEST`), then dropped into a bare in-kernel debug
   shell.
2. **The terminal felt empty** — that default in-kernel shell has only a handful
   of debug builtins (`ping`, `mem`, ...), and even the user-space `/bin/sh`
   couldn't run `ls`/`uname` by name: it `execve`'d `argv[0]` verbatim with no
   `PATH` search, so a bare command silently failed (and, worse, a failed
   `execve` in the child returned back into the shell loop, spawning duplicate
   prompts).

## 2. What changed

### Boot (`kernel/kernel.c`)
- The boot-time self-tests and their diagnostics are now **off by default** and
  gated behind `makh.verbose`. No coverage is lost: the same subsystems are
  exercised by the headless suite (`makh.test`) and from the shell's `selftest`
  command.
- The load-bearing init that used to hide *inside* `test_timer()` — `pic_init`,
  `timer_init`, `ktime_init_realtime`, `syscall_init`, `idt_enable_interrupts` —
  now runs unconditionally; only the smoke test and the 2-second check are
  verbose-only (`test_timer()` → `test_timer_checks()`).
- **Default interactive boot now launches the user-space shell** (`/bin/sh`) with
  the keyboard + line discipline live. `keyboard_init()` (previously reached only
  through the keyboard self-test) runs on every interactive boot, so typing
  actually works. The small built-in kernel debug shell is still available with
  `makh.kshell`; `makh.sh` remains accepted and is equivalent to the default.
- `[ELF] loaded ...` dropped from INFO to DEBUG so it no longer prints between
  every command in the terminal.

### Shell (`user/sh.c`)
- **`PATH` search:** a bare command name is looked up in `/bin` then `/usr/bin`,
  so `ls`, `uname`, `cat`, ... run without the full path. A name containing `/`
  is used as-is.
- **Not-found is reported and fatal in the child:** on a failed exec the child
  prints `sh: <cmd>: not found` and `exit(127)` instead of returning into the
  read-eval loop (which was spawning phantom extra prompts).
- **Builtins:** `cd [dir]` (default `/`) and `exit [code]`, which must run in the
  shell process itself.

### Native coreutils (`user/ls.c`, `user/cat.c`, `user/pwd.c`)
The shipped busybox is a minimal build, so most `/bin/<applet>` symlinks resolved
to a busybox that doesn't contain that applet. MAKH now ships its own small
coreutils, staged as real `/bin/ls`, `/bin/cat`, `/bin/pwd` (removed from the
busybox symlink list in the `Makefile`; `ls`/`cat`/`pwd` no longer collide):

- **`ls`** — lists a directory one name per line (dotfiles hidden) via
  `getdents64`; a file argument prints its own name; no argument lists `.`.
- **`cat`** — copies each file argument (or stdin) to stdout.
- **`pwd`** — prints `getcwd`.

With `echo` (already native), `uname`/`true`/`false`/`sleep` (busybox), and the
`cd`/`exit` builtins, the default terminal is now genuinely usable.

## 3. Verification

- **183 in-kernel tests pass**; `make stress` (24×4) clean. `busybox.applets_via
  _symlink` still passes (the `ash`/`true`/`false` symlinks are kept; only
  `ls`/`cat`/`pwd` became native).
- **Visual (QMP screendump, framebuffer):** default `makhos.iso` boots to the
  shell in a few seconds with a short, clean log; `pwd`, `ls /`, `ls /bin`,
  `cat /etc/motd`, `uname -a` and `echo` all produce correct output, echoed and
  rendered on the framebuffer console.

## 4. Flags

| cmdline       | effect                                                        |
|---------------|---------------------------------------------------------------|
| *(none)*      | quiet boot → user-space `/bin/sh`                             |
| `makh.sh`     | same as default (kept for compatibility)                      |
| `makh.kshell` | the built-in kernel debug shell (`ping`/`arp`/`mem`/`fuzz`/…) |
| `makh.verbose`| restore the brick-by-brick boot diagnostics + smoke tests     |
| `makh.test`   | headless self-test suite, then power off (CI)                 |
| `makh.debug`  | KLOG level = DEBUG                                             |
