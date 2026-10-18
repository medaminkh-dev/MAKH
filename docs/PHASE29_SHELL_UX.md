# MakhOS Phase 29: shell UX — cwd prompt, TAB completion, redirection, heredocs, colour grep

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the interactive `/bin/sh` now shows the working directory
in its prompt, completes commands and paths with TAB, redirects with
`>`/`>>`/`<` and heredocs (`<<EOF`), and `grep` highlights matches in red on a
terminal. **183 in-kernel tests.**
**Depends on:** 28 (interactive line editor), 26 (fast boot + `/bin/sh`),
18 (VFS/getdents64), 25/FB-1 (framebuffer console + ANSI colour)

---

These are UX "patch" bricks on the road to 2.0.0: no kernel changes, all in the
ring-3 shell and coreutils, so the kernel and its 183 KTESTs are untouched.

## 1. Prompt shows the working directory

`build_prompt()` (user/sh.c) calls `getcwd()` and builds
`makh:<cwd>$ ` before every line, with the name in cyan and the path in green
(`\033[1;36m` / `\033[1;32m`). The SGR escapes are zero-width, so the line
editor's `\r`-based redraw math is unchanged — it reprints the literal prompt
each keystroke, and the cursor still lands at `visible_prompt_cols + cursor`.
The user no longer has to run `pwd` to know where they are, and `cd` is
reflected immediately (verified: `cd /bin` → prompt becomes `makh:/bin$`).

## 2. TAB completion

A `Tab` (byte 9) branch in the line editor completes the token under the cursor:

- **Command position** (first word, or first after a `|`) with no `/`: scan
  `/bin` and `/usr/bin` for names with the typed prefix.
- **Otherwise** (an argument): split the token into a directory part and a base,
  and scan that directory (or the cwd) via `getdents64`, skipping `.`/`..`.

It inserts the **longest common prefix** of the matches, appends `/` for a sole
directory match or a space for a sole file/command match, and on an ambiguous
TAB with no further common prefix it lists the candidates and redraws. Each
candidate directory is read with `getdents64` into a 2 KiB buffer.

## 3. Redirection: `>`, `>>`, `<`

`strip_redirs()` pulls `>`, `>>` and `<` (and their targets, whether `> f` or
`>f`) out of an argv in place, filling a `redir_t`. `apply_redirs()` runs in the
child after fork: it `open()`s the targets and `dup2()`s them onto fd 0/1
(`O_WRONLY|O_CREAT|O_TRUNC`, or `O_APPEND` for `>>`). Redirections are parsed per
pipeline stage, so they compose with pipes and an explicit redirect overrides
the pipe end. The kernel already preferred a VFS file description over the
console for a redirected fd, so no kernel change was needed.

## 4. Heredocs: `<<EOF`

`expand_heredoc()` runs on the whole line before the pipe split. On `<<DELIM` it
reads the body from the terminal (`sh_readline` with a `> ` continuation prompt)
until a line equal to `DELIM`, writes it to `/tmp/.heredoc`, and rewrites the
line as `... < /tmp/.heredoc ...`. So `cat > f <<EOF` writes a file straight
from the terminal, and because the rewrite produces an ordinary `<` redirection
it also works before a pipe (`cat <<EOF | grep x`). `^D` during the body aborts
the command.

## 5. Colour `grep`

`grep` highlights each match in bright red (`\033[1;31m … \033[0m`) when stdout
is a terminal (detected with `TCGETS`); piped or redirected, output stays plain,
so scripts and the test harness are unaffected. Plain runs between matches are
flushed in one write each.

## 6. Verification

- **183 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
- **Interactive (QMP send-key):** the prompt tracked `cd /bin`; `whoam`+TAB ran
  `whoami` → `root`; `echo … > /tmp/rt` then `>>` then `cat` showed both lines;
  `grep redir /tmp/rt` printed the match in red; `cat /tmp/r`+TAB completed the
  path; `cat > /tmp/hd <<END` / `heredoc-works` / `END` then `cat /tmp/hd`
  printed `heredoc-works`.
- The shell KTESTs (`feed` + exit-status) are unaffected — no test asserts the
  prompt, `strip_redirs` is a no-op without redirection operators, and Tab is
  never fed.

## 7. Not in this brick (next)

- `chmod` with real per-file modes (needs a mutable vnode mode + a `chmod` op).
- A text editor (nano/vim-like).
- `wget`/`curl`/`nc` once a ring-3 socket syscall layer exists.
- Quoting/escapes in the tokenizer (so `echo "a > b"` keeps the `>` literal).
