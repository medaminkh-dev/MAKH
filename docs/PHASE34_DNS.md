# MakhOS Phase 34: DNS — curl/wget/nc take hostnames

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — the network commands resolve names, so `curl example.com`
works, not just `curl 10.0.2.2`. **184 in-kernel tests.**
**Depends on:** 32 (ring-3 sockets), 14 (UDP)

---

## 1. A resolver in user space

`user/uresolve.h` is a small header-only DNS client shared by `curl`, `wget` and
`nc`. `uresolve(host, &ip)`:

1. returns immediately if `host` is already a dotted-quad;
2. otherwise opens a **connected UDP socket** (the ring-3 socket layer — a
   `SOCK_DGRAM` with `connect` so `write`/`read` go to the nameserver), sends one
   **A-record query**, and parses the first A record from the answer (following
   the one compression pointer the question/answer names use).

No new kernel networking was needed beyond one option: a UDP `recv` blocks
forever without a timeout, which would hang a command whose DNS server is silent.
So this brick adds a minimal **`setsockopt`** syscall (54) for `SO_RCVTIMEO`;
the length is inferred from the option name, which keeps it on the 4-argument
syscall path. The resolver sets a 4-second receive timeout, so a lost query
fails cleanly ("cannot resolve") instead of hanging.

## 2. Choosing the nameserver

`uresolve` reads **`/etc/resolv.conf`** for a `nameserver X.X.X.X` line (an
optional `:PORT` is accepted, handy for testing against a local responder), and
falls back to **10.0.2.3** — QEMU slirp's built-in resolver — when the file is
absent. No caching, search domains or IPv6: just name → address.

## 3. The commands

`curl`, `wget` and `nc` now take a name or an IP for the host; `curl`/`wget` also
send a correct `Host:` header. Still HTTP/1.0 and no TLS (`https://` is a later
brick).

```
curl http://example.com/           wget http://example.com/index.html
nc example.com 80
```

## 4. Verification

- **184 in-kernel tests pass**; `make stress` (24×4) clean; license headers ok.
- **Interactive (QMP send-key)**, against a throwaway DNS responder on the host
  (answering any A query with 10.0.2.2) reached from the guest via slirp, plus
  an HTTP server on 10.0.2.2:8000:
  - `echo nameserver 10.0.2.2:5300 > /etc/resolv.conf`, then
    `curl http://makh.test:8000/test.txt` **and** `curl http://second.host:...`
    (a different name) both resolved and fetched the body — real name
    resolution, not a hardcoded address;
  - pointing the resolver at a dead port, `curl http://foo.bar:...` printed
    "cannot resolve" after the 4 s timeout and the shell carried on — **no
    hang**;
  - dotted-quad hosts still work unchanged.

## 5. Not in this brick (next)

- TLS, so `https://` works.
- A resolver cache and `search`/`domain` handling; IPv6 (AAAA).
- `send`/`recv`/`sendto`/`recvfrom`/`bind`/`listen`/`accept` syscalls for musl
  and for ring-3 servers.
