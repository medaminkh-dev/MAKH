# MakhOS Phase 35: a visible cursor + friendlier network commands

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the terminal and the editor draw a cyan block cursor so
you can see where you are typing, and `curl`/`wget`/`nc`/`ping` take a bare
hostname with no scheme or port to remember. **184 in-kernel tests.**
**Depends on:** 34 (DNS), 31 (editor), 28 (raw-mode line editor), 43 (FB console)

---

## 1. Scope

Two usability bricks, no new kernel surface beyond one SGR code:

- a **visible cursor** on the framebuffer console — in the shell's line editor
  and in `edit` — because the FB graphics console has no blinking hardware
  cursor the way the legacy VGA text mode did;
- `curl`/`wget`/`nc`/`ping` **handle the scheme and port themselves**, so
  `curl yahoo.com`, `curl https://yahoo.com/`, `nc host` and `ping yahoo.com`
  all work without typing `http://…:80`.

## 2. A cyan block cursor (kernel/vga.c, user/sh.c, user/edit.c)

The framebuffer console draws glyphs into pixels; it never had the blinking
cell cursor of VGA text mode, so in raw-mode programs (the shell line editor,
`edit`) you could not see where the next character would land.

The fix is a painted cursor. The FB console's SGR handler gains **reverse
video** (`\033[7m` / `\033[27m`, codes 7 and 27): it swaps the current
foreground and background before setting the cell colour. That alone makes the
editor's status bar a proper bar, and it is the mechanism a cursor needs.

Both line editors now paint the cell under the cursor as a **cyan block**
(`\033[30;46m` — black on cyan) after each redraw, then reset:

- `user/sh.c` `refresh()` draws the block at the cursor column; the prompt shows
  it the moment it appears, and on **Enter** the line is redrawn once *without*
  the block so a finished command leaves no cyan artifact in the scrollback.
- `user/edit.c` paints the same block at the text cursor (the status bar stays
  reverse-video).

Because the glyph blitter fills the whole cell background, a space on a cyan
background is a solid block and a character on it is drawn black-on-cyan — a
cursor you can always see, and it tracks arrow-key movement.

## 3. Scheme and port are optional (user/curl.c, user/wget.c, user/nc.c)

`parse_url` in `curl`/`wget` now skips **any** `scheme://` prefix (not just
`http://`) by scanning the leading `[a-zA-Z0-9+-.]*` and a following `://`, so:

```
curl yahoo.com            curl http://yahoo.com/        curl https://yahoo.com/x
curl host:8080/path       wget example.com              wget 10.0.2.2:8000/f
```

all parse to the right host, port and path. The **port defaults to 80** (an
explicit `:port` wins; a `:0` falls back to 80). There is no TLS, so an
`https://` URL is served as **plain HTTP on port 80** rather than refused — the
user does not have to know or care which scheme a site wants.

`nc HOST [PORT]` makes the **port optional too** (defaults to 80); an explicit
port is still validated (1–65535).

## 4. ping takes a hostname (user/ping.c)

`ping` now resolves its target through the same `uresolve()` the network
commands use, so `ping yahoo.com` works, not just `ping 10.0.2.2`. The header
mirrors real `ping`, showing the name and the address it resolved to:

```
PING yahoo.com (74.6.231.21): 4 echo requests, 56 data bytes
```

A dotted-quad is still accepted unchanged (`uresolve` returns it immediately),
and a name that will not resolve prints `ping: cannot resolve host`.

## 5. Verification

- **184 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
- **Cursor (QMP send-key + screendump):** the shell prompt shows the cyan block;
  typing then pressing Left arrows moves the block onto the right character
  (verified over the middle of a word); `edit` shows the block at the insertion
  point with a reverse-video status bar; a completed command line is left with
  no leftover block.
- **Network (QMP send-key), against a throwaway DNS responder (any A → 10.0.2.2)
  and an HTTP server on 10.0.2.2:8000, reached from the guest via slirp:**
  - `curl makh.test:8000/test.txt` (no scheme) and
    `curl https://makh.test:8000/test.txt` (https) both fetched the body;
  - `ping 10.0.2.2 2` and `ping makh.test 2` both got 2/2 replies, the latter
    printing `PING makh.test (10.0.2.2)` — the hostname resolved end to end.
- The `parse_url` scheme/port logic was also unit-tested on the host across
  bare names, `http`/`https`/`ftp` schemes, explicit and zero ports, and IPs.

## 6. Not in this brick (next)

- Real TLS so `https://` is actually encrypted (today it is plain HTTP).
- A resolver cache and `search`/`domain` handling; IPv6 (AAAA).
- `send`/`recv`/`bind`/`listen`/`accept` syscalls for ring-3 servers and musl.
