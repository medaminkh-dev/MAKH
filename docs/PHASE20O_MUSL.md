# MakhOS Phase 20-O (F20): a real C program on musl libc

**Version:** 0.1.0-dev
**License:** AGPL-3.0-only (or commercial, see LICENSING.md)
**Status:** COMPLETE — an ordinary C program, compiled against **musl libc** as a
static-PIE and run on MAKH unchanged, reaches `main`, uses TLS/heap/stdio, and
exits cleanly. **158 in-kernel tests.**
**Depends on:** 20-H (FS base / TLS), 20-L (`clone`/`futex`/`set_tid_address`),
20-B (`brk`/`mmap`), 20-N (`writev`/`exit_group`), 20-E (the SysV initial stack)

---

## 1. Scope

F20-a (20-N) closed the last *syscall* gaps a static libc touches. This brick is
F20-b: actually **compiling musl and running a program linked against it**, which
is the milestone the whole 20-H…20-N arc was built toward.

- **Toolchain.** musl is built from source by the `ziglang` package (`pip install
  ziglang`), which vendors the complete musl tree and cross-compiles it on demand.
  `zig cc -target x86_64-linux-musl` is a real musl toolchain, obtained without any
  host-specific dependency. The program `user/musl/hello.c` and the regeneration
  rule `make musl-progs` are in-tree; the linked binary is **checked in** at
  `user/musl/muslhello` so the normal build and CI need no extra toolchain — it is
  copied into the initrd as `/bin/muslhello`.

- **Why static-PIE.** MAKH places every user image in one PML4 slot at a 35 TiB
  base (`0x0000_2000_0000_0000`). A non-PIE link there overflows the small code
  model's 32-bit relocations, and zig's prebuilt musl `crt1.o` + `libc.a` are
  small-model (≈1400 `R_X86_64_32`), so they cannot be re-based by hand. A
  **static-PIE** (`-fPIE -pie -static`) carries only `R_X86_64_RELATIVE`
  relocations and relocates *itself* at startup (musl's `rcrt1`/`dlstart`), so it
  loads at any base. That is the mechanism this brick adds support for.

---

## 2. The ELF loader learns ET_DYN (`proc/elf.c`)

`elf_load()` previously accepted only a fixed `ET_EXEC`. It now also loads an
`ET_DYN` (PIE) at a fixed **load bias** (`USER_PIE_BASE`, the bottom of the
window):

- every `p_vaddr` is interpreted relative to the bias — segments, the entry
  (`*entry = bias + e_entry`), and the program headers — with the same
  overflow-safe window checks as before, now applied to `bias + p_vaddr`;
- it hands back an `elf_aux_t` describing the image for the auxiliary vector:
  `AT_PHDR` (the in-memory address of the program headers, from `PT_PHDR` or the
  `LOAD` segment covering `e_phoff`), `AT_PHENT`, `AT_PHNUM`, and the bias
  (`AT_BASE`).

W^X is unchanged and still holds: the one executable `LOAD` is R+X, and a PIE's
self-relocations and RELRO all target writable (`RW+NX`) segments, so the binary
fixes itself up without the loader ever mapping a page W+X.

## 3. The initial stack grows an auxiliary vector (`proc/user.c`)

`setup_user_stack()` now emits the auxv entries a real libc reads, not just the
three a freestanding crt needed:

- **`AT_PHDR`/`AT_PHENT`/`AT_PHNUM`** — musl walks these to self-relocate and to
  find its `PT_TLS` template (`__init_tls`);
- **`AT_BASE`** — the load bias;
- **`AT_RANDOM`** — a pointer to 16 fresh bytes (from `krandom`) placed near the
  top of the stack, which musl uses to seed the stack canary;
- **`AT_PAGESZ`/`AT_ENTRY`** as before.

`proc_spawn_user()` also now passes the program path as `argv[0]` (`argc == 1`),
the SysV convention a C runtime expects even with no further arguments.

## 4. `sched_getaffinity` (`syscall/syscall.c`)

musl probes the CPU set once at startup. MAKH is single-CPU (SMP is a later
track), so `sched_getaffinity(204)` reports exactly `{CPU 0}`: it writes one
online bit and returns the byte count, and musl zero-fills the rest of its
`cpu_set_t`.

## 5. The bug this brick surfaced: a lost FS base across a context switch

The program faulted on its first `%fs:0` access even though musl had called
`arch_prctl(ARCH_SET_FS)` with a valid thread pointer. The cause was in
`context_switch` (`arch/context_switch.asm`): on the load path it reloaded the
**FS selector** (`mov fs, ax`), and in long mode loading a selector into `fs`
**resets that segment's base MSR to 0** — undoing the base `arch_prepare_switch`
pins on every switch, a slot earlier.

GS already had this exact treatment (it is deliberately *not* reloaded, for the
`swapgs` invariant); FS had the same hazard but was missed. It stayed **latent
since Phase 20-H**: MAKH's own tiny TLS test never had a context switch fall
between its `SET_FS` and its first TLS read. musl's startup — dozens of syscalls,
preemptible — reliably lands a switch in that window, so the base was zeroed and
the TLS read took a `#PF` at linear address 0.

**Fix:** stop reloading the FS selector, exactly like GS. The FS base is owned by
`arch_prepare_switch` (re-pinned from the PCB on every switch) and by
`arch_prctl`; the selector is vestigial in long mode. One deleted `mov fs, ax`.

## 6. Two smaller fixes made along the way

- **User stdout reaches the serial console.** `write(fd 1/2)` fell back to
  `terminal_putchar`, which only draws to VGA — the kernel's own logs reach the
  serial console through a different path (`terminal_writestring`), so a user
  program's output was invisible on serial (and so headless/in the cloud). The
  fd 1/2 fallback now goes through `terminal_write`, which mirrors to the serial
  console as well. This is why the musl program's `printf` line now appears in
  the boot log, and it is a prerequisite for an interactive shell over serial.
- **KFUZZ never leaves the console muted.** The shell-fuzz target mutes the
  console (`terminal_set_quiet(1)`) while it hammers thousands of commands; a
  caught fault used to `longjmp` past its un-mute. `kfuzz.c:run_one` now clears
  the flag after every target, on both the normal return and the fault path.

## 7. Fuzzed

- **`kfuzz.target_elf`** now also drives the `ET_DYN` path: the generator mutates
  `e_type`, so garbage PIE headers with a biased `p_vaddr` reach the new code.
  The oracle is unchanged — strict page conservation across create→load→destroy —
  and the biased window checks keep every segment inside the per-process slot.

## 8. Tests (`kernel/tests/test_musl.c`) + program (`user/musl/hello.c`)

- **`musl.static_pie_hello_runs`** (`/bin/muslhello`) — exits **42** iff crt
  startup + self-relocation, TLS via `%fs` (a `__thread` variable read and
  written), the heap (`malloc`/`free`), and buffered stdio succeeded. The program
  gates its own exit on `printf`'s return and `fflush`'s success, so **42 proves
  the `writev`(fd 1) flush reached the kernel** — the test does not rely on
  visible console output.

**158 in-kernel tests pass**; `make stress` (serial) is clean.

## 9. Deferred to F20-c

Boot a static `busybox sh` (built the same way) and run a shell script on MAKH —
a much larger exercise of the same ABI (many more syscalls, real signal use,
`wait`/pipe-heavy). Dynamic linking (`ld.so`, shared objects) is a separate, much
later track and is **not** required for the self-hosting goal.
