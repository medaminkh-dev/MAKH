# MakhOS Phase 31: `edit` — a small full-screen text editor

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — MAKH has a native full-screen text editor: open a file,
move around, type, save and quit, all on the framebuffer console. **184
in-kernel tests.**
**Depends on:** 28 (raw-mode tty via `TCGETS`/`TCSETS`), 25/FB-1 (framebuffer
console + ANSI cursor control), 18 (VFS file I/O)

---

MAKH's own editor — a tiny `nano`. No new kernel code: it is a ring-3 program
built on the raw-mode terminal and the ANSI cursor/erase sequences the console
already understands.

## 1. Model

`user/edit.c` keeps the file as a line array (`line[MAXLINES][MAXCOL]` +
`llen[]` + `nlines`) and a cursor `(cx, cy)` in text coordinates, plus a `top`
scroll offset. On launch it reads the file into the array (an absent file starts
empty), switches the tty to raw mode (clearing `ICANON|ECHO` via `TCSETS`, saved
and restored on exit), and redraws after every keystroke.

## 2. Screen

`draw()` homes the cursor (`ESC[H`), paints the visible window — lines
`top .. top+23`, each erased to end of line (`ESC[K`) and truncated at 80
columns — and a reverse-video (`ESC[7m`) status bar on row 25 showing the file
name, a `[modified]` flag and the key hints, then positions the hardware cursor
with `ESC[row;colH`. It assumes an 80x25 console (no window-size query yet) and
scrolls vertically for taller files; `scroll_fix()` keeps the cursor line in
view.

## 3. Keys

| Key | Action |
|---|---|
| printable | insert at the cursor |
| Enter | split the line at the cursor |
| Backspace / Delete | delete left / delete at cursor (joins lines at an edge) |
| ← → ↑ ↓ | move (wrapping to the previous/next line at an edge) |
| Ctrl-A / Ctrl-E | start / end of line |
| Ctrl-S | save (writes the lines joined by `\n`) |
| Ctrl-Q / Ctrl-X | quit (restores the tty and clears the screen) |

## 4. Verification

- **184 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
  (No KTEST drives the editor — it is interactive — but adding it to the initrd
  leaves the suite green.)
- **Interactive (QMP send-key, with a new Ctrl-key path in the test driver):**
  `edit /tmp/ed` opened the full-screen view with the status bar; typing
  `hello-editor`, Enter, `second-line` showed `[modified]`; **Ctrl-S** cleared
  `[modified]` (saved); **Ctrl-Q** restored the shell; `cat /tmp/ed` then printed
  both lines back, confirming the save.

## 5. Not in this brick (next)

- A window-size query (`TIOCGWINSZ`) so the editor uses the real console size
  instead of a fixed 80x25, plus horizontal scrolling for long lines.
- Search, go-to-line, and undo.
