# MakhOS Phase 33: `cc` — a friendly C compiler driver

**Version:** 1.0.0
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — `cc hello.c` (and `tcc hello.c`) now compile and link a C
program in one line; the raw TinyCC lives at `/bin/tcc.real`. **184 in-kernel
tests.**
**Depends on:** 21-A/B (tcc on MAKH), 21-C (make), the musl sysroot at `/usr`

---

## 1. The problem

TinyCC on MAKH could already compile C (Phase 21), but only when invoked with
the full musl link line — the self-host demo and KTESTs call it as
`tcc -nostdinc -I/usr/include -nostdlib -static -L/usr/lib /usr/lib/crt1.o …`.
A newcomer who just typed `tcc hello.c` hit:

```
tcc: error: file 'crt1.o' not found
tcc: error: file 'crti.o' not found
```

because stock tcc looks for the C runtime objects on its *built-in* paths, which
do not point at MAKH's `/usr` sysroot. The compiler worked; the ergonomics did
not.

## 2. The driver

`user/cc.c` is a small driver installed as **both `/bin/cc` and `/bin/tcc`**; the
real TinyCC moves to **`/bin/tcc.real`**. For a plain compile it expands the
command to exactly what the self-host build uses:

```
tcc.real -nostdinc -I/usr/include -nostdlib -static -L/usr/lib \
         /usr/lib/crt1.o /usr/lib/crti.o  <your args>  \
         /usr/lib/crtn.o -lc /usr/lib/libtcc1.a  -o <out>
```

- `cc hello.c` → `./hello` (output name defaults to the source basename minus
  `.c`; `a.out` if there is no `.c` argument).
- `cc -o prog hello.c` → `./prog`.

If you pass your own toolchain flags — `-c`, `-nostdlib`, `-nostdinc`, `-I`,
`-L`, `-B`, `-E`, `-S`, `-run`, `-print-search-dirs`, `-v` — the driver detects
that you are steering the compiler yourself and `exec`s `/bin/tcc.real`
unchanged. So the self-hosting build (`make` + the `-nostdlib` invocations) and
the F21 KTESTs run exactly as before, just one `exec` deeper.

## 3. Verification

- **184 in-kernel tests pass** — including the three F21 self-host tests, which
  now reach TinyCC through the wrapper's pass-through; `make stress` (24×4)
  clean; license headers ok.
- **Interactive (QMP send-key):**
  - `cc /share/tcc-full.c -o /tmp/h` then `/tmp/h` →
    `hello from a full libc program on MAKH, 42`;
  - `tcc /share/tcc-full.c -o /tmp/h2` (the wrapper as `tcc`) → the same;
  - `cc /share/tcc-full.c` with no `-o` produced `tcc-full` (a 38 KB executable,
    confirmed with `ls -l`) which ran and printed the message — so the default
    output name works.

## 4. Not in this brick (next)

- Marking the output executable (MAKH's loader ignores the x bit, so compiled
  programs already run; this is cosmetic).
- Multiple-source one-liners already work (`cc a.c b.c -o app`); a real
  dependency-aware driver is what `make` is for.
