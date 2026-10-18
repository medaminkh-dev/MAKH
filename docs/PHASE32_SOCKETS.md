# MakhOS Phase 32: ring-3 sockets — `curl`, `wget`, `nc`

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — user programs can open TCP connections: `curl`, `wget`
and `nc` fetch and relay over the real e1000 stack. **184 in-kernel tests.**
**Depends on:** 14 (TCP/IP + BSD sockets), 18 (VFS fd layer), 22 (MMIO window),
20-K (pipe fd model, the template for this bridge)

---

The kernel has had a full BSD socket layer since Phase 14, but only in-kernel
code (and the KTESTs) could use it: socket descriptors were indices into a
private `socks[]` table, disconnected from the per-process VFS fd table, so a
ring-3 program had no way to reach them. This brick bridges the two so ordinary
user programs do real networking.

## 1. A socket is a vnode

The bridge mirrors how pipes (Phase 20-K) expose a kernel object as an fd:

- `VNODE_SOCK` is a new vnode type whose `priv` holds the `socks[]` index.
- `sockv_ops` maps the file operations onto the socket API: `read` → `recv`,
  `write` → `send` (both returning `-errno` on error). So once a socket has an
  fd, the normal `read`/`write`/`close`/`dup2` paths just work — no special
  cases in the fd layer.
- `net_socket_open(type)` opens a kernel socket, wraps it in a `VNODE_SOCK`
  vnode, and installs it with the new `vfs_install_vnode()` helper (which
  allocates a `file_t` + fd for any pre-built vnode).
- `file_put()` (the VFS close path) calls `sock_on_vnode_close()` when the last
  fd on a socket vnode goes away, so closing the fd runs `sock_close()`
  (TCP teardown + slot free).

## 2. Two new syscalls

Only two calls are new, because read/write/close already route through the
vnode:

- **`socket(2)` (41)** → `net_socket_open` — returns a normal fd.
- **`connect(2)` (42)** → copies in the `sockaddr_in` and calls the kernel
  `connect()` on the underlying socket.

A ring-3 client is then: `socket()`, `connect()`, `write()` the request,
`read()` the reply, `close()`. `send`/`recv` as distinct syscalls are
unnecessary (they are just `write`/`read` here).

## 3. The commands

- **`curl [-i] URL`** — HTTP/1.0 GET, prints the body (or, with `-i`, the whole
  response) to stdout.
- **`wget [-O file] URL`** — the same GET, saved to a file (default: the last
  path component, else `index.html`).
- **`nc IP PORT`** — a tiny netcat: `fork()`s so one side copies stdin → socket
  and the other copies socket → stdout, i.e. full-duplex raw TCP.

No DNS or TLS yet, so the host is a dotted-quad IP and the scheme is `http://`.

## 4. Verification

- **184 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
- **Interactive (QMP send-key), against a throwaway HTTP server reached through
  QEMU slirp at `10.0.2.2:8000`:**
  - `curl http://10.0.2.2:8000/test.txt` printed the body (headers stripped);
  - `wget …/test.txt` saved the file (`cat` showed the body back);
  - `nc 10.0.2.2 8000` with a GET fed over a heredoc returned the raw
    `HTTP/1.0 200 OK …` response.

## 5. Not in this brick (next)

- `bind`/`listen`/`accept` for ring-3 servers (`nc -l`), and `send`/`recv`/
  `sendto`/`recvfrom`/`shutdown`/`setsockopt` syscalls for musl.
- DNS resolution and TLS (`https://`).
- `poll`/`select` over socket fds (the pollable-fd work, U3).
