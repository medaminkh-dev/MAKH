# MAKH 1.2.0 — the fuzzer earns its keep 🦊🔒

**MakhOS** is a from-scratch x86-64 operating system kernel, written in
freestanding C and assembly, built **brick by brick** — a phase lands only when
its tests are green. 1.2.0 is a **security release**: two external
static-analysis passes found real bugs in 1.1.0, every one is fixed here, and
the kernel's own self-fuzzer (KFUZZ) was rebuilt so it now **reproduces most of
them with a replay seed** — the whole point of having a fuzzer. **199 in-kernel
tests passing**, a 24×4 stress run clean, CI green.

## The headline

A fuzzer whose pitch is "the kernel finds its own bugs" had been outscored by
someone *reading the source*. 1.2.0 is the answer: of the 11 findings across two
audits, **8 are now caught by KFUZZ v2**, each proven by reverting the fix and
watching the fuzzer report the bug with a deterministic seed. The other 3 are
fixed and regression-tested while the fuzzer grows the reach to cover them.

The full story — why KFUZZ v1 was blind to each finding, and the v1↔v2
scorecard — is in **`docs/KFUZZ_V2_STATIC_ANALYSIS.md`**. The per-bug advisory
is in **`docs/SECURITY_ADVISORY_1.2.0.md`**.

## Security fixes

### P1 — unprivileged input compromises or panics the kernel
- **AUD-01** — `copy_to_user` to a read-only/unmapped user page faulted on the
  **store** half of the copy loop, which the fixup didn't cover, panicking the
  kernel. The fixup now recovers load **and** store faults → `-EFAULT`.
- **AUD-03** — `sigreturn` trusted an attacker-controlled RIP/RSP (a
  non-canonical RIP `#GP`s in ring 0 after `swapgs`, the CVE-2012-0217 shape)
  and let a process mask `SIGKILL`/`SIGSTOP` and become unkillable. Both are now
  validated / forced.
- **AUD-05** — ext2 mount used an untrusted superblock's geometry raw:
  `1024 << log_bs` (shift UB) and divide-by-`inodes_per_group` (`#DE`). A
  geometry gate now runs before any shift or division.
- **F1** — an ext2 directory name from disk overflowed a kernel-stack buffer;
  the copy is now bounded.
- **F2** — `mprotect`/`munmap` accepted kernel-half addresses and walked the
  shared kernel page tables; both now reject any non-user range.

### P2 — leaks / resource exhaustion
- **F3** — overlapping ELF `PT_LOAD` segments leaked a frame in `vmspace_map`.
- **F4** — `fork`'s page-table copy leaked frames on an OOM rollback.
- **F5** — `kheap_expand` leaked pages when a later map failed.
- **AUD-07** — the ELF loader orphaned a segment frame when `vmspace_map` failed.

### Hardening
- **AUD-04** — the ELF loader now rejects an entry point outside any executable
  segment.
- **F6** — `kfuzz_longjmp` restores the interrupt flag captured at `setjmp`
  instead of an unconditional `sti`.

## KFUZZ v2 — the self-fuzzer, rebuilt

- **v2.0 — honor the verdict.** The campaign driver now reads each target's
  self-check return instead of discarding it. (It immediately surfaced a latent
  `strchr` bug.)
- **v2.1 — fault injection.** `pmm_test_fail_after(n)` makes an allocation fail
  mid-operation, so OOM-rollback paths (fork, heap, mmap) are finally reachable;
  a new `oom` target drives them against a strict page-conservation oracle.
- **v2.2 — adversarial addresses.** `t_uvm` throws kernel-half, non-canonical,
  page-0, and overflowing addresses at `mprotect`/`munmap`.
- **v2.3 — multi-segment ELF.** The `t_elf` generator emits two `PT_LOAD`s that
  may share a page.
- **v2.4 — three new reaches:** `t_uaccess` (adversarial `copy_to/from_user`),
  `t_ext2geo` (adversarial ext2 geometry), and ELF-loader fault injection inside
  `t_oom`.

Scorecard: **8 of 11 findings** are now fuzzer-caught with replay seeds; see the
static-analysis doc for the table and the honest gaps (F1, AUD-03, AUD-04 remain
fix+KTEST only, pending a RAM-disk and a userspace-fuzzing harness).

## Assets

| File | What it is |
|---|---|
| `makhos-1.2.0.iso` | Bootable ISO — `qemu-system-x86_64 -cdrom makhos-1.2.0.iso -vga std -m 128M` |
| `MAKH-1.2.0-src.tar.gz` | Full source tree (`make && make run`) |
| `MAKH-1.2.0-src.zip` | Full source tree (zip) |
| `SHA256SUMS` | Checksums for the three files above |

## Build from source

Needs `gcc`, `nasm`, `grub-mkrescue` (with `xorriso`) and `qemu-system-x86_64`.

```sh
tar xzf MAKH-1.2.0-src.tar.gz && cd MAKH-1.2.0
make            # build makhos.kernel + makhos.iso
make run        # boot it in QEMU
make test       # run the 199 in-kernel tests
make stress     # 24×4 stress campaign
fuzz 2000       # drive KFUZZ v2 over all targets from the shell
```

## License

Dual-licensed: **GNU AGPL-3.0** (see `LICENSE`) or a **commercial license**
(see `LICENSING.md`). Earlier revisions were BSD-3-Clause; see `NOTICE`.
