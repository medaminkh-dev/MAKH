<div align="center">

  <img src="https://res.cloudinary.com/dqtzw37rg/image/upload/v1772868531/MAKH-Vector_dbnspj.png" alt="MAKH Logo" width="620">

  <p><strong>MAKH Ain't a Kernel Hack</strong></p>
  <p>An experimental from-scratch operating system for kernel architecture research, CPU meta-awareness models, and fuzzing-driven security experimentation.</p>

  <!-- GitHub Badges -->
  <p>
    <a href="https://github.com/medaminkh-dev/MAKH/blob/main/LICENSE">
      <img src="https://img.shields.io/badge/license-BSD%203--Clause-blue.svg" alt="License: BSD 3-Clause">
    </a>
    <a href="https://github.com/medaminkh-dev/MAKH/commits/main">
      <img src="https://img.shields.io/github/last-commit/medaminkh-dev/MAKH" alt="Last Commit">
    </a>
    <a href="https://github.com/medaminkh-dev/MAKH/issues">
      <img src="https://img.shields.io/github/issues/medaminkh-dev/MAKH" alt="Open Issues">
    </a>
    <a href="https://github.com/medaminkh-dev/MAKH/stargazers">
      <img src="https://img.shields.io/github/stars/medaminkh-dev/MAKH?style=social" alt="GitHub Stars">
    </a>
    <a href="https://github.com/medaminkh-dev/MAKH/network/members">
      <img src="https://img.shields.io/github/forks/medaminkh-dev/MAKH?style=social" alt="GitHub Forks">
    </a>
  </p>

  <!-- Tech Stack Badges -->
  <p>
    <img src="https://img.shields.io/badge/arch-x86__64-red" alt="Arch: x86_64">
    <img src="https://img.shields.io/badge/phase-v0.0.2%20(User%20Mode)-brightgreen" alt="Phase: v0.0.2">
    <img src="https://img.shields.io/badge/bootloader-Multiboot2-orange" alt="Bootloader: Multiboot2">
    <img src="https://img.shields.io/badge/language-C-00599C?logo=c" alt="Language: C">
    <img src="https://img.shields.io/badge/language-Assembly-6E4C13" alt="Language: Assembly">
    <img src="https://img.shields.io/badge/build-Makefile-427819" alt="Build: Makefile">
  </p>

  <!-- Project Nature Badges -->
  <p>
    <img src="https://img.shields.io/badge/OS-Bare%20Metal-black" alt="Bare Metal">
    <img src="https://img.shields.io/badge/emulator-QEMU-red" alt="Emulator: QEMU">
    <img src="https://img.shields.io/badge/toolchain-GCC%20Cross%20Compiler-blue" alt="Toolchain: GCC">
    <img src="https://img.shields.io/badge/security-fuzzing--driven-critical" alt="Fuzzing Driven">
    <img src="https://img.shields.io/badge/research-kernel%20architecture-blueviolet" alt="Research">
    <img src="https://img.shields.io/badge/status-active%20development-brightgreen" alt="Status: Active">
  </p>

  <!-- Repo Stats Badges -->
  <p>
    <img src="https://img.shields.io/github/repo-size/medaminkh-dev/MAKH" alt="Repo Size">
    <img src="https://img.shields.io/github/languages/top/medaminkh-dev/MAKH" alt="Top Language">
    <img src="https://img.shields.io/github/commit-activity/m/medaminkh-dev/MAKH" alt="Commit Activity">
    <img src="https://img.shields.io/badge/contributions-welcome-brightgreen" alt="Contributions Welcome">
  </p>

  <hr>

</div>

## 📖 Overview

MAKH is not just another "toy OS". It is a deep-dive research platform built from the ground up to explore:

- **Kernel Architecture**: Hands-on implementation of core OS concepts (memory management, interrupts, scheduling) on bare metal.
- **CPU Meta-Awareness**: Experimentation with low-level CPU features, privilege rings (Ring 0/3), and hardware-assisted security models.
- **Fuzzing-Driven Security**: Designed as a testbed for developing and validating kernel fuzzing techniques to discover vulnerabilities.

The project is meticulously documented and developed in a **"brick-by-brick"** fashion, with every phase tracked in versioned releases.

---

## 🚀 Features & Current State (v0.0.2)

The kernel has successfully completed its first two major versions, establishing a solid foundation.

### ✅ Phase 1: Bootstrap & Bare Metal (v0.0.1)

- Custom `x86_64` boot sequence using **Multiboot2** for GRUB compatibility.
- Seamless transition from 32-bit protected mode to 64-bit long mode.
- Basic VGA text mode output for early debugging.

### ✅ Phase 2: Memory Management & User Mode (v0.0.2)

- **Physical Memory Manager (PMM)**: Bitmap-based allocator for 4KB frames.
- **Virtual Memory Manager (VMM)**: Complete 4-level paging with identity mapping and recursive page tables.
- **Kernel Heap (`kmalloc`/`kfree`)**: A dynamic memory allocator for kernel objects.
- **Interrupt Handling**: Full IDT setup with handlers for CPU exceptions, IRQs (PIC), and system calls.
- **Device Drivers**: PIT (Timer) and PS/2 Keyboard drivers with circular buffers.
- **Process Foundation**: Basic Process Control Blocks (PCB) and a round-robin context switch.
- **System Calls**: Implemented using `int 0x80` with a syscall table (`syscall.c`).
- **User Mode Transition**: Successfully jumps to **Ring 3** and executes a user program that makes `write` and `exit` syscalls! (See `user_program.asm`).

---

## 🧱 Project Structure & Philosophy

This project follows a **strict "brick-by-brick" methodology**. Each feature is built, verified, and committed in isolation. The repository structure reflects this clarity:

```
MAKH/
├── boot/          # Bootloader (boot.asm, Multiboot2 header)
├── docs/          # Versioned documentation (0.0.1, 0.0.2)
├── kernel/        # Core kernel source tree
│   ├── arch/      # Architecture-specific code (x86_64)
│   ├── drivers/   # Hardware drivers (keyboard, timer, serial)
│   ├── include/   # Kernel headers
│   ├── lib/       # Internal kernel libraries (string, etc.)
│   ├── mm/        # Memory management (PMM, VMM, KHeap)
│   ├── proc/      # Process management (PCB)
│   ├── syscall/   # System call implementations
│   └── user/      # User-space programs (e.g., user_program.asm)
├── Makefile       # Build system
├── linker.ld      # Linker script
└── grub.cfg       # GRUB configuration for ISO creation
```

---

## 🛠️ Build & Run

Building MAKH requires a cross-compilation environment for `x86_64`.

### Prerequisites

- `gcc`, `ld` (or `x86_64-elf-gcc` / `x86_64-elf-ld`)
- `nasm` (Netwide Assembler)
- `make`
- `grub-mkrescue` (for creating bootable ISOs)
- `qemu-system-x86_64` (for emulation)

### Build Instructions

1. **Clone the repository:**
   ```bash
   git clone https://github.com/medaminkh-dev/MAKH.git
   cd MAKH
   ```

2. **Build the kernel and ISO:**
   ```bash
   make clean
   make iso
   ```

3. **Run in QEMU:**
   ```bash
   make run
   ```
   > To debug with GDB: `make debug`

---

## 📚 Documentation & Roadmap

All major phases are documented in the `docs/` directory. This provides a clear, historical record of the project's evolution.

- `docs/0.0.1/` — Phase 1: Bootstrap & Bare Metal
- `docs/0.0.2/` — Phase 2: Memory Management & User Mode
  - Detailed reports for sub-phases (PIC/Timer, Keyboard, GDT/TSS, Syscalls, Processes, User Mode).

### 🗺️ What's Next? (v0.0.3)

The immediate focus is on **Process Management & Multitasking**:

- Full process lifecycle (`fork`, `wait`, `exit`).
- Priority-based scheduler.
- Process isolation with Copy-on-Write (COW).
- Inter-Process Communication (IPC) primitives (Signals, Pipes).

---

## 🤝 Contributing

This is a research-oriented project, but discussions and collaborations are welcome! If you're interested in kernel development, OS security, or fuzzing, feel free to:

- **Open an Issue** to discuss a feature, bug, or research idea.
- **Fork the repository** and experiment on your own branch.

---

## 📄 License

This project is licensed under the **BSD 3-Clause License**. See the [LICENSE](LICENSE) file for details.

---

## ✨ Acknowledgements

MAKH was built phase by phase, with every implementation decision
grounded in primary hardware documentation and community knowledge.

### 📖 Primary Technical References

- **[Intel® 64 and IA-32 Architectures Software Developer's Manual](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)**
  — The ground truth for everything in MAKH. Directly referenced for:
  x86_64 long mode transition, PML4/PDPT/PD paging structures, GDT/TSS
  128-bit descriptors, privilege rings (Ring 0/3), EFER/STAR/LSTAR/FMASK MSRs,
  `syscall`/`sysret` mechanics, and `iretq` frame layout.

- **[AMD64 Architecture Programmer's Manual, Volume 2: System Programming](https://www.amd.com/en/support/tech-docs)**
  — Referenced for `syscall`/`sysret` native x86_64 design, MSR configuration
  (IA32_STAR, IA32_LSTAR, IA32_FMASK), and the EFER.SCE bit enabling.

- **[OSDev Wiki](https://wiki.osdev.org)**
  — The most referenced community resource throughout all phases.
  Used for: Multiboot2 header format, 8259A PIC remapping (0x20–0x2F),
  PIT divisor calculation, PS/2 keyboard scancode set 1, IDT gate setup,
  context switching, and GDT/TSS descriptor formats.

- **[Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)**
  — Followed precisely to implement the Multiboot2 header
  (magic: `0xe85250d6`) and parse GRUB's memory map for the PMM.

- **[System V AMD64 ABI](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf)**
  — Followed for calling conventions across all phases, and specifically
  for syscall argument register order (`RAX`, `RDI`, `RSI`, `RDX`, `R10`,
  `R8`, `R9`) — Linux x86_64 compatible.

- **[NASM Documentation](https://www.nasm.us/doc/)**
  — Essential reference for all assembly files: `boot.asm`,
  `syscall_asm.asm`, `context_switch.asm`, `usermode.asm`,
  `idt_asm.asm`, and `user_program.asm`.

- **[GNU Linker (LD) Manual](https://sourceware.org/binutils/docs/ld/)**
  — Used to craft `linker.ld` for kernel section layout, user program
  sections (`.user_text`, `.user_data`), and correct load address at `0x100000`.

- **[QEMU Documentation](https://www.qemu.org/docs/master/)**
  — Used throughout all phases for emulation, GDB stub (`make debug`),
  and hardware behavior verification.

### 🏗️ Inspired By

- **[xv6 (MIT)](https://github.com/mit-pdos/xv6-public)** — Influenced
  the structural approach to process management, PCB design, round-robin
  scheduling, and the syscall table architecture.

- **[Linux Kernel](https://github.com/torvalds/linux)** — Referenced for
  real-world implementations of the privilege mechanisms explored in MAKH,
  particularly the `syscall`/`sysret` path and page table layout.
