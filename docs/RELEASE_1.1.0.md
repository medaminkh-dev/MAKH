# MAKH 1.1.0 — the terminal grows up 🦊

**MakhOS** is a from-scratch x86-64 operating system kernel, written in
freestanding C and assembly, built **brick by brick** — a phase lands only when
its tests are green. 1.1.0 builds on 1.0.0 (tag `v1.0.0`) with a real
interactive terminal, network client tools, a text editor, and one-line C
compilation, all on the same discipline: **184 in-kernel tests passing**, a
24×4 stress run clean, and CI green.

## What's new since 1.0.0

### Shell & terminal (phases 29, 35)
- **A prompt that shows the working directory** — `makh:/home$` — in colour.
- **TAB completion** for commands (from `PATH`) and for files and directories.
- **Redirection and heredocs** — `>`, `>>`, `<`, and `<<EOF … EOF`.
- **`grep` highlights matches in red** on a terminal.
- **A visible cyan cursor** in the shell and the editor, since the framebuffer
  graphics console has no blinking hardware cursor.

### A text editor (phase 31)
- **`edit FILE`** — a small full-screen editor (ring-3, raw-mode): arrow-key
  movement, insert/delete, line split/join, `^S` save, `^Q`/`^X` quit, with a
  status bar and the cyan cursor.

### Networking from user space (phases 32, 34, 35)
- **Ring-3 BSD sockets** — a `VNODE_SOCK` bridge so `socket`/`connect` and
  `read`/`write`/`close` reach the kernel TCP/UDP stack (plus `setsockopt` for a
  receive timeout).
- **`curl`, `wget`, `nc`** — an HTTP client, a downloader and a netcat.
- **DNS** — a userspace resolver so `curl`/`wget`/`nc`/`ping` take **hostnames**,
  not just IPs; reads `/etc/resolv.conf`.
- **Scheme and port are optional** — `curl yahoo.com`, `curl https://yahoo.com/`
  and `host:8080/path` all work (no TLS yet, so `https` is served as plain HTTP),
  and **`ping yahoo.com`** resolves the name.

### File permissions (phase 30)
- **`chmod`** — real, mutable file modes: a vnode mode + a `.chmod` VFS op,
  honoured by `tmpfs` (in memory) and `ext2` (persisted), with `chmod`/`fchmodat`
  syscalls and a native `chmod` command.

### Self-hosting C (phase 33)
- **`cc hello.c`** / **`tcc hello.c`** — a friendly compiler driver that compiles
  **and links** against the musl sysroot in one line (raw TinyCC stays at
  `/bin/tcc.real`).

## Bug fixes
- **`tcc file.c` failed with `crt1.o` / `crti.o` not found** — the new `cc`/`tcc`
  driver wires the sysroot's crt objects and libraries, so a plain compile just
  works.
- **Stress flake in `job.ctrl_c_kills_job_not_shell`** — synchronisation margins
  widened so a heavily loaded 24×4 run no longer races the scheduler.
- **Compiler `-Wall` misleading-indentation warnings** in the new network
  commands cleaned up.

## Assets

| File | What it is |
|---|---|
| `makhos-1.1.0.iso` | Bootable ISO — `qemu-system-x86_64 -cdrom makhos-1.1.0.iso -vga std -m 128M` |
| `MAKH-1.1.0-src.tar.gz` | Full source tree (`make && make run`) |
| `MAKH-1.1.0-src.zip` | Full source tree (zip) |
| `SHA256SUMS` | Checksums for the three files above |

## Build from source

Needs `gcc`, `nasm`, `grub-mkrescue` (with `xorriso`) and `qemu-system-x86_64`.

```sh
tar xzf MAKH-1.1.0-src.tar.gz && cd MAKH-1.1.0
make            # build makhos.kernel + makhos.iso
make run        # boot it in QEMU
make test       # run the 184 in-kernel tests
```

Boot, watch the fennec splash, then try `help`, `edit notes.txt`,
`curl example.com`, or `ping example.com` at the `$` prompt.

## License

Dual-licensed: **GNU AGPL-3.0** (see `LICENSE`) or a **commercial license**
(see `LICENSING.md`). Earlier revisions were BSD-3-Clause; see `NOTICE`.
