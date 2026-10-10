# MakhOS Phase 28: interactive shell — history, keyboard shortcuts, coreutils

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the ring-3 `/bin/sh` now has a raw-mode line editor with
**command history** (Up/Down), cursor movement and kill/erase shortcuts, backed
by a `termios` the shell can flip to raw mode; and `/bin` carries a full set of
native commands. **183 in-kernel tests.**
**Depends on:** 26 (fast boot + `/bin/sh`), 19 (TTY line discipline),
20-F (job control), 25/FB-1 (framebuffer console)

---

## 1. Why

The shell read a whole line through the kernel's **canonical** (ICANON) line
discipline: usable, but with no history and no in-line editing — an arrow key
printed a stray escape, and there was no way to recall the previous command.
A real interactive shell edits the line *itself*. That means the shell must own
the line while it is being typed, which in turn means two things the kernel did
not yet expose to ring 3:

1. a way to put the terminal in **raw mode** (no ICANON, no ECHO), and
2. **navigation keys** delivered as bytes the shell can parse.

## 2. Raw mode: `termios` + `TCGETS`/`TCSETS`

`kernel/tty/tty.c` already kept a `termios_t` (`c_lflag` + `c_cc[]`). Two
Linux-compatible ioctls now expose it to user space (`kernel/syscall/syscall.c`,
`do_ioctl`):

- **`TCGETS` (0x5401)** — copy the current `termios` out to the caller.
- **`TCSETS` (0x5402)** — install a new `termios`.

`user/usys.h` declares a byte-compatible `struct termios` (same
`uint32 c_lflag; uint8 c_cc[8]` layout) and the `ICANON`/`ECHO`/`ISIG` bits with
the **same values as the kernel** (`0x0002`/`0x0008`/`0x0001`), plus thin
`ugettermios`/`usettermios` wrappers. The shell clears `ICANON|ECHO`, edits and
echoes the line itself, and restores the saved `termios` before every child
runs — so programs still see a normal cooked terminal.

### Raw-mode read path

Canonical mode assembles a whole line into one buffer and hands it over on
Enter. Raw mode needs byte-at-a-time delivery, so `tty.c` grows a small
**byte ring** (`raw_q[256]`, head/tail):

- `tty_input()` in `!ICANON` pushes each byte into the ring (dropping on
  overflow) and wakes readers.
- `tty_read()` in raw mode drains the ring. If the ring is empty it **bridges**
  any line that was assembled in canonical mode *before* the shell switched to
  raw (and EOF), delivering it byte-wise — so input that arrives during the
  switch, and the way the in-kernel KTEST harness feeds the shell canonically,
  both keep working unchanged.

This bridge is what lets the existing shell KTESTs pass against a shell that now
reads in raw mode: **183/183**, no test was relaxed.

## 3. Navigation keys → ANSI escapes

`kernel/drivers/keyboard.c` handles extended (`0xE0`) scancodes. When a ring-3
shell owns the terminal (`tty_is_active()`), the arrows and Delete are now
delivered as the conventional escape sequences the shell parses:

| Key | Bytes |
|---|---|
| ↑ ↓ → ← | `ESC [ A` / `B` / `C` / `D` |
| Delete | `ESC [ 3 ~` |

When no ring-3 shell is active (the in-kernel shell / legacy path) the old
`keyboard_buffer_write(CHAR_ARROW_*)` behaviour is untouched.

## 4. The line editor (`user/sh.c`, `sh_readline`)

`sh_readline(prompt, buf, max)` draws and edits the line. On a non-tty
(a pipe or a script, detected when `ugettermios` fails) it falls back to a plain
`uread`, so pipelines and scripts are unaffected.

- **History** — a ring of the last 32 entries (`HIST_MAX`), consecutive
  duplicates skipped. **Up** walks older, **Down** walks newer; the partially
  typed line is **stashed** on the first Up and restored when Down returns to the
  bottom.
- **Movement** — Left/Right, **Ctrl-A** (home), **Ctrl-E** (end).
- **Editing** — insert at the cursor, **Backspace**/**Delete**, **Ctrl-U** (kill
  line), **Ctrl-K** (kill to end), **Ctrl-D** (EOF on an empty line).
- **Redraw** — `refresh()` rewrites the line with `\r`, the prompt, the buffer
  and **`ESC[K`** (erase to end of line), then repositions the cursor. The
  `ESC[K` handler was added to both the framebuffer console
  (`fb_console_clear_to_eol`) and the VGA-text fallback (`kernel/vga.c`).

## 5. The commands in `/bin`

The shell is only as useful as its commands. `/bin` now carries native,
freestanding implementations (each a standalone `umain` program, no libc):

| Area | Commands |
|---|---|
| Files | `ls` (`-l`/`-a`, symlink-aware via `lstat`), `cat`, `cp`, `mv`, `ln -s`, `rm`, `mkdir`, `rmdir`, `touch`, `pwd` |
| Text | `echo`, `grep`, `head`, `tail`, `wc`, `sort`, `cut` |
| System | `whoami`, `id`, `uname`, `env`, `date`, `clear`, `sleep` |
| Network | `ping` (ttl + rtt min/avg/max), `ifconfig` |
| Guide | `help` — an A-to-Z of MAKH OS and its commands |

Two kernel bugs were found and fixed while testing these **adversarially**,
command by command:

- **`mv` / `rename`** — `tmpfs` had no `.rename` op, so `vfs_rename` returned
  `-EINVAL` and `mv` always failed. Added `tmpfs_rename` (handles an existing
  destination, including a non-empty-directory `-ENOTEMPTY`, and a no-op
  same-vnode rename) and wired it into `tmpfs_ops`.
- **`ls -la` on a symlink** — `SYS_LSTAT` was aliased to `do_stat`, which
  *follows* links, so a symlink showed its target's metadata. Added a true
  no-follow `vfs_lstat`, an `S_IFLNK` case in `fill_stat`, and `do_lstat`; `ls`
  now shows `lrwxrwxrwx` and the `-> target`.

`whoami`/`id` answer the "am I ring-3 or root?" question (MAKH is single-user:
uid/gid 0, root), and `ifconfig` lists interfaces via a new `SYS_MAKH_IFINFO`.

## 6. Verification

- **183 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
- **Interactive (QMP send-key):** typing commands then **Up/Up** recalled and
  re-ran an earlier command (serial confirmed the recalled command executed);
  **Down** walked history forward; **Left** + insert edited mid-line
  (`echo helo` → `echo hello`). The `ESC[K` redraw leaves no stale characters.
- Every `/bin` command was exercised with valid and **invalid** input
  (missing args, bad paths, non-existent files); the shell survives each error
  and returns to the prompt.

## 7. Not in this brick (next)

- A user-space `wget` and a user-space socket `ping`: both need a
  `socket`/`connect`/`send`/`recv` syscall layer exposed to ring 3.
- Tab-completion and reverse-search (Ctrl-R) in the line editor.
- Exposing the ring-0 subsystems (fuzzer, selftest, mem/ps) as user commands.
