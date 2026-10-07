# MakhOS Phase 20-D: controlling terminal & a shell

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — a user process blocks reading the terminal, Ctrl+C kills
the foreground program, and a tiny `/bin/sh` runs commands; boot `makh.sh` for
an interactive prompt. 140 in-kernel tests
**Depends on:** Phase 19 (line discipline, process groups), Phase 20-A/A-2
(user processes, fork/execve/wait4)

---

## 1. Scope

This is the last of gap #1 that stands between the process model and an actual
prompt. It wires the keyboard to a blocking read and puts a shell on top:

- **blocking `read()` on the controlling terminal** — a user process reading
  stdin sleeps until a line is typed, then gets it;
- **Ctrl+C** — the `^C` raises `SIGINT` on the foreground process group and the
  fatal signal is delivered on the way back to ring 3, killing the program;
- **`/bin/sh`** — a tiny shell: prompt → read a line → `fork`+`execve` it →
  `wait` → repeat;
- **`makh.sh` boot** — start `/bin/sh` as the first interactive process with the
  keyboard feeding the line discipline.

**Deferred:** job control (each command in its own process group + `tcsetpgrp`,
so Ctrl+C hits only the running job and not the shell), user-installed signal
handlers (`sigaction`/`sigreturn`), argument parsing (the shell runs a bare
command path — `argv` arrives with musl), and line editing/history on the user
side (the kernel line discipline already does erase).

---

## 2. Blocking terminal read (`tty/tty.c`, `syscall.c`)

The Phase-19 line discipline buffered a completed line but a reader had to poll.
Now a wait queue sits behind it: `tty_read_blocking()` sleeps on it until a line
(or EOF) is available, and `line_complete()` wakes it. `read(fd 0)` routes here,
so a user process calling `read(0, …)` blocks until the user presses Enter and
then receives the line; `^D` on an empty line is a zero-length read (EOF). A
signal while blocked returns `-EINTR` (and §3 then delivers it).

## 3. Fatal signals delivered on return to ring 3 (`signal.c`, `syscall.c`, `idt.c`)

Phase 19 delivered signals cooperatively to kernel threads. A *running user
process* now gets them too: `signal_check_and_die()` runs on the way back to
ring 3 — both at syscall return and when the timer (or the keyboard IRQ)
interrupts a process in ring 3 — and if a pending, unblocked, default-terminate
signal is waiting, the process exits with `128 + signo`. That is the whole
path behind **Ctrl+C**: keyboard → `^C` → `SIGINT` to the foreground group →
delivered on return → the program dies with 130. (The IRQ-path check only fires
when ring 3 was interrupted, never mid-syscall, so no kernel work is cut short;
user-installed handlers are a later brick.)

## 4. `/bin/sh` (`user/sh.c`)

A deliberately small shell, to prove the loop rather than to be featureful:

```
for (;;) {
    write(1, "$ ");
    n = read(0, line);            // blocks on the tty
    if (n <= 0) exit(last);       // ^D ends the session
    pid = fork();
    if (pid == 0) execve(line);   // child becomes the command
    waitpid(pid, &last);          // parent waits
}
```

A line is a command path (absolute or relative to the cwd); arguments are not
parsed yet. This is exactly `fork`+`execve`+`wait4` driven by terminal input.

## 5. Booting into the shell (`kernel.c`, `drivers/keyboard.c`)

With `makh.sh` on the kernel command line, `kernel_main` (already PID 1) marks
the terminal active, starts `/bin/sh` in ring 3, sets it as the foreground
group, and idles — the scheduler runs the shell. While the terminal is active
the keyboard IRQ feeds keystrokes through the line discipline
(`Ctrl+<letter>` → the control byte, so `^C`/`^D` work) instead of the legacy
in-kernel line editor, so the default boot is unchanged.

## 6. Tests (`kernel/tests/test_term.c`) + programs (`user/`)

Input is injected from the kernel via `tty_input()` — the exact entry point the
keyboard IRQ uses — so the whole path is exercised headless:

- **`read_blocks_until_a_line_arrives`** (`/bin/readline`) — a process blocked
  in `read()` wakes and returns the line once it is "typed".
- **`ctrl_c_kills_the_foreground_reader`** — `^C` on the foreground group kills
  the reader with `128 + SIGINT` (130).
- **`ctrl_d_on_empty_line_is_eof`** — `^D` is a zero-length read.
- **`shell_runs_a_typed_command`** (`/bin/sh`) — the shell reads `/bin/hello`,
  fork+execve+waits it, and on EOF exits with its status (42) — the whole
  interactive loop in miniature.

The `makh.sh` boot is additionally smoke-tested: it starts the shell and stays
alive with no fault. **140 in-kernel tests pass**; `make stress` is clean.

## 7. Deferred

| Item | Why it waits |
|---|---|
| job control (per-command pgroup, `tcsetpgrp`) | so Ctrl+C hits the job, not the shell |
| user signal handlers (`sigaction`/`sigreturn`) | needs a signal frame on the user stack |
| `argv`/`envp` parsing in the shell | needs argv at execve (the musl brick) |
| `pipe`/`dup` and redirection | the next slice of the syscall surface |

With this, gap #1 (the process model) is **functionally closed**: a user can
boot to a prompt and run programs. What remains for a musl/busybox userland is
`argv`/`auxv`, the wider syscall surface (`pipe`/`dup`/`stat`/`getdents`), and
persistent storage.
