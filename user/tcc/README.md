# tcc — the self-hosting C compiler (F21)

`tcc` is a build of [TinyCC](https://repo.or.cz/tinycc.git) checked in as a
prebuilt, the same way `user/musl/busybox` is. It runs **on MAKH** as an
ordinary static-PIE user program and compiles C straight to a runnable
executable — its assembler and linker are built in, so no external tools are
invoked. This is the first brick of F21 (self-hosting): MAKH building and
running a program with a real compiler, on itself.

## License

TinyCC is **LGPL-2.1** — a separate work from MAKH (AGPL-3.0-only), shipped here
only as a convenience binary. Its source and license live upstream; this repo
vendors just the compiled `tcc` so the normal build and CI need no extra
toolchain (exactly the arrangement used for the musl/busybox prebuilts).

## How it was built

On a host with the `ziglang` pip package (the same musl cross-toolchain the
other prebuilts use), via the `zcc` wrapper (zig cc, static-PIE musl, dropping
the GNU-ld-only flags lld rejects):

```
git clone https://repo.or.cz/tinycc.git && cd tinycc
./configure --cc=<path>/zcc --cpu=x86_64
make tcc
strip tcc           # 3.5 MB -> ~0.7 MB; still a static-PIE musl ELF
```

The result is a `DYN` (PIE) musl ELF that loads at the slot-64 bias like the
other static-PIE programs. It ships into the initrd as `/bin/tcc`.

## What MAKH does with it

`kernel/tests/test_f21.c` has MAKH run:

```
tcc -nostdlib -static -o /tmp/tcc-out /share/tcc-hello.c
```

`-nostdlib -static` keeps the output freestanding (no libc/crt needed), and tcc
links it as a standard **non-PIE `ET_EXEC` at `0x400000`** — the conventional
low address that the higher-half kernel (Phase 21) made loadable. MAKH then
execs `/tmp/tcc-out`, which prints its marker and exits 42.

`user/tcc/hello.c` is that freestanding source (its own `_start` + inline
syscalls). A full libc-linked compile needs musl + crt + `libtcc1.a` staged on
the MAKH filesystem — the next step on the road to rebuilding MAKH on itself.
