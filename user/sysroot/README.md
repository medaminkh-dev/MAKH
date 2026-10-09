# sysroot — a musl `/usr` for compiling libc programs on MAKH (F21-b)

This is a checked-in `/usr` tree that lets `tcc`, **running on MAKH**, compile
and link ordinary C programs against a real C library. The `$(INITRD)` recipe
copies `usr/` into the boot tmpfs at `/usr`; `kernel/tests/test_f21.c` then has
MAKH run `tcc … -I/usr/include /usr/lib/crt1.o … -lc /usr/lib/libtcc1.a …`.

```
usr/include/      musl's public headers (the full tree)
usr/lib/libc.a    static C library (stripped)
usr/lib/crt1.o    _start → __libc_start_main → main
usr/lib/crti.o    .init/.fini prologue
usr/lib/crtn.o    .init/.fini epilogue
usr/lib/libtcc1.a tcc's compiler-runtime helpers
```

## Licenses (all separate works from MAKH)

- **musl** (`include/`, `libc.a`, the crt objects) — **MIT**. MakhOS is
  AGPL-3.0-only; musl is vendored here only as a convenience, exactly like the
  `user/musl` and `user/tcc` prebuilts. Source and full text live upstream
  (<https://musl.libc.org>).
- **`libtcc1.a`** — **LGPL-2.1**, part of TinyCC; see `user/tcc/README.md`.

## How it was built (host, once)

With the ziglang musl cross-toolchain used by the other prebuilts:

```
# musl 1.2.6 — headers + static libc.a + crt objects
./configure --prefix=/tmp/muslroot CC="<path>/zcc" --disable-shared
make && make install
strip -S /tmp/muslroot/lib/libc.a          # debug strip; still a valid archive

# libtcc1.a — tcc's runtime, from the same tinycc tree as user/tcc/tcc
make -C tinycc libtcc1.a

# assemble the tree checked in here
mkdir -p usr/include usr/lib
cp -r  /tmp/muslroot/include/*          usr/include/
cp     /tmp/muslroot/lib/{libc.a,crt1.o,crti.o,crtn.o}  usr/lib/
cp     tinycc/libtcc1.a                 usr/lib/
```

Only a handful of headers are strictly needed for the test program, but the full
header tree is shipped so MAKH can compile arbitrary C, not just the test —
the point of F21 is a system that can build software for itself.
