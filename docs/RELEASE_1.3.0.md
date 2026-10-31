# MAKH 1.3.0 — the fuzzer reaches the last three 🦊🎯

**MakhOS** is a from-scratch x86-64 operating system kernel, written in
freestanding C and assembly, built **brick by brick** — a phase lands only when
its tests are green. 1.3.0 finishes the job 1.2.0 started: the three audit
findings that were *fixed and regression-tested but not yet fuzzer-reachable*
are now **driven by the fuzzer and the ring-3 harness end to end**. **204
in-kernel tests passing**, a 24×4 stress run clean.

## The headline

1.2.0 was honest about a gap: of 11 static-analysis findings, 8 were caught by
KFUZZ v2, and **3 (F1, AUD-03, AUD-04) were fix+KTEST only** because reaching
them faithfully needed machinery the kernel hadn't built. 1.3.0 builds that
machinery and closes all three:

- **AUD-04** — `t_elf` gained a **known-verdict oracle**: the generator emits
  ELF images whose correct accept/reject it knows, so an entry point one byte
  past the executable segment is a verdict mismatch the target reports. *(landed
  first in this series.)*
- **F1** — **`t_ext2img`** mounts the test's own ext2 image through a
  copy-on-write **block-device overlay** (`kernel/include/fs/blkdev.h`), mutates
  a few metadata sectors, and walks it into a guarded buffer. The 200-byte name
  under `/longdir` drives the `readdir` bound every clean mount; a clobbered
  guard byte is an oracle failure. Supporting bricks: a `blkdev` abstraction that
  decouples ext2 from virtio, and `ext2_unmount()` so a fuzz campaign's repeated
  mount/unmount doesn't leak.
- **AUD-03** — **`/bin/sigfuzz`**, a ring-3 adversary, forges the saved
  `sigcontext` from inside a signal handler and lets the real `rt_sigreturn`
  run. Reverting the RFLAGS sanitise or the `SIGKILL`/`SIGSTOP` mask-strip flips
  its exit status (true oracles); the non-canonical RIP/RSP rejection asserts the
  safe end-to-end outcome but can't flip under QEMU-TCG — the ring-0 SYSRET `#GP`
  it guards (CVE-2012-0217) is Intel-silicon behaviour the emulator doesn't
  model, so the validator's logic stays pinned by its white-box KTEST.

Honest scorecard: **10 of 11 findings flip when their fix is reverted**, and the
11th (AUD-03's non-canonical rejection) is pinned end-to-end plus by a white-box
test — named precisely rather than rounded up. The full story and the v1↔v2↔v2.5
table are in **`docs/KFUZZ_V2_STATIC_ANALYSIS.md`**.

## Also in 1.3.0

- **Early IDT.** The interrupt descriptor table is now loaded right after the
  command line, *before* the first memory-touching init (the multiboot scan,
  PMM, heap, framebuffer map). A fault in that early, firmware/bootloader-fed
  window is now a diagnosable CPU exception (vector + RIP + CR2, and a FAIL exit
  under `makh.test`) instead of a silent triple fault with no IDT to catch it.
- **Build.** `make` with no target now builds the whole OS again
  (`.DEFAULT_GOAL := all`); it previously fell through to the first user-program
  rule.
- **CI compiler pin.** The runner's default `gcc` tracks the newest packaged
  GCC, which broke the build twice: GCC 14 made `-Wreturn-mismatch` a hard error
  (a test helper is fixed), and a still-newer GCC isolates an early `pmm_init()`
  path to a `ud2` that #UD-triple-faults at boot — a path GCC 13 and GCC 14 both
  compile to correct, passing code (204/204), so it reads as compiler
  over-aggression on sound code, not a reproduced defect. CI now pins the
  validated GCC 13 for reproducibility; the early-IDT change above is what turned
  that previously-silent triple fault into the diagnosable exception dump that
  located it. Root-causing it against a local GCC 15 is tracked follow-up.

## Assets

| File | What it is |
|---|---|
| `makhos-1.3.0.iso` | Bootable ISO — `qemu-system-x86_64 -cdrom makhos-1.3.0.iso -vga std -m 128M` |
| `MAKH-1.3.0-src.tar.gz` | Full source tree (`make && make run`) |
| `MAKH-1.3.0-src.zip` | Full source tree (zip) |
| `SHA256SUMS` | Checksums for the three files above |

## Build from source

Needs `gcc`, `nasm`, `grub-mkrescue` (with `xorriso`) and `qemu-system-x86_64`.

```sh
tar xzf MAKH-1.3.0-src.tar.gz && cd MAKH-1.3.0
make            # build makhos.kernel + makhos.iso
make run        # boot it in QEMU
make test       # run the 204 in-kernel tests
make stress     # 24×4 stress campaign
fuzz 2000       # drive KFUZZ over all targets from the shell
```

## License

Dual-licensed: **GNU AGPL-3.0** (see `LICENSE`) or a **commercial license**
(see `LICENSING.md`). Earlier revisions were BSD-3-Clause; see `NOTICE`.
