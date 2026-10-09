# make — GNU make running on MAKH (F21-c)

`user/make/make` is [GNU make](https://www.gnu.org/software/make/) 4.4.1, built
as a static-PIE musl binary and checked in as a prebuilt — the same arrangement
as `user/tcc/tcc` and the `user/musl` programs. It runs **on MAKH** as an
ordinary static-PIE user program and drives `tcc` across a multi-file build: it
reads a `Makefile`, works out what to compile, and fork/execs the compiler for
each target. It ships into the initrd as `/bin/make`.

Because every recipe in `proj/Makefile` is a plain absolute-path `tcc` command
with no shell metacharacters, make runs each one directly (fork + execve), so no
`/bin/sh` is required on MAKH for this build.

## The demo project (`proj/`)

`proj/` is a deliberately multi-file C project — `main.c` + `greet.c` + a shared
`greet.h` — with a `Makefile` that compiles each `.c` to a `.o` with `tcc` and
links the objects with musl's crt + `libc.a` into a single static executable.
It ships at `/share/mkproj`; `kernel/tests/test_f21.c` has MAKH run
`make -C /share/mkproj`, then runs the resulting `/share/mkproj/app` (exit 42).

`TCC` and `SYSROOT` in the Makefile default to MAKH's own paths (`/bin/tcc`,
`/usr`) but are overridable, so the exact same project also builds on a host
(the prebuilt tcc is an ordinary `x86_64-linux-musl` binary):

```
user/make/make -C user/make/proj \
    TCC=$PWD/user/tcc/tcc SYSROOT=$PWD/user/sysroot/usr
```

## License

GNU make is **GPL-3.0-or-later** — a separate work from MAKH (AGPL-3.0-only),
shipped here only as a convenience binary; its source and license live upstream.
This repo vendors just the compiled `make` so the normal build and CI need no
extra toolchain (the same arrangement used for the tcc/musl/busybox prebuilts).

## How it was built

On a host with the `ziglang` pip package (the musl cross-toolchain the other
prebuilts use), via the `zcc` wrapper (zig cc, static-PIE musl):

```
curl -O https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz
tar xzf make-4.4.1.tar.gz && cd make-4.4.1
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC=<path>/zcc CFLAGS="-Os"
make
strip make          # a static-PIE musl ELF, ~0.3 MB
```
