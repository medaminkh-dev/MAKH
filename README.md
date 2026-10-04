<div align="center">

```
 ███╗   ███╗ █████╗ ██╗  ██╗██╗  ██╗
 ████╗ ████║██╔══██╗██║ ██╔╝██║  ██║
 ██╔████╔██║███████║█████╔╝ ███████║
 ██║╚██╔╝██║██╔══██║██╔═██╗ ██╔══██║
 ██║ ╚═╝ ██║██║  ██║██║  ██╗██║  ██║
 ╚═╝     ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝
```

### An x86-64 kernel that tests itself — from the inside, in ring 0.

[![CI](https://github.com/medaminkh-dev/MAKH/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/medaminkh-dev/MAKH/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![Commercial license](https://img.shields.io/badge/commercial-available-success.svg)](LICENSING.md)
![Arch](https://img.shields.io/badge/arch-x86--64-informational)
![Language](https://img.shields.io/badge/C%20%2B%20asm-freestanding-lightgrey)
![Tests](https://img.shields.io/badge/in--kernel%20tests-82%20passing-brightgreen)

[Quick start](#-quick-start) · [Highlights](#-highlights) · [KFUZZ](#-kfuzz-the-kernel-fuzzes-itself) · [Architecture](#-architecture) · [Docs](#-documentation) · [License](#-license)

</div>

---

**MakhOS** is a from-scratch operating system kernel for x86-64, written in
freestanding C and assembly. It boots with GRUB, schedules threads
preemptively, speaks TCP/IP over a real NIC driver — and it carries its own
**coverage-guided fuzzer that attacks the kernel from inside ring 0**,
surviving the faults it provokes and printing the exact seed to reproduce
each one.

It is built **brick by brick**: every phase lands only when its tests are
green.

```console
MakhOS> ping 10.0.2.2 3
PING 10.0.2.2: 3 echo requests
reply from 10.0.2.2: seq=1 time=250 ms
reply from 10.0.2.2: seq=2 time=100 ms
reply from 10.0.2.2: seq=3 time=100 ms
--- 10.0.2.2: 3 sent, 3 received, 0% loss
MakhOS> fuzz netrx 300
fuzzing 300 iterations...
done: 300 iters, 0 crashes, 0 oracle-fails, coverage 184 edges, corpus 2
```

## ✨ Highlights

<table>
<tr>
<td width="50%" valign="top">

### 🧵 Preemptive multitasking
Priority run queues, quantum-based preemption at the IRQ tail, sleep,
wait queues with timeouts, and an idle reaper for detached threads.

### 🧩 Clean POSIX threads
`pthread_create/join/detach`, normal / recursive / error-checking mutexes,
condition variables, semaphores with `sem_timedwait`, TLS keys and a
per-thread `errno`.

### 🛡️ Self-checking memory
A kernel heap with a full integrity walker, stack canaries checked on every
context switch, a process-tree invariant checker, and x86 hardware
watchpoints for hunting stray writes.

</td>
<td width="50%" valign="top">

### 🌐 Real TCP/IP stack
PCI + Intel e1000 driver over DMA, ARP, IPv4, ICMP, UDP and a full RFC 793
TCP (retransmission, fast retransmit, flow control, TIME_WAIT) behind a
POSIX socket API. 200 KB over loopback TCP in ~20 ms; survives 1-in-7
packet loss byte-exact.

### 🔬 KFUZZ self-fuzzer
Coverage-guided fuzzing of the kernel's own heap, page allocator, string
routines, pthreads, shell and network receive path — with a sandbox that
turns ring-0 faults into reproducible reports.

### 🖥️ Built-in shell
`ifconfig` · `arp` · `ping` · `netstat` · `ps` · `mem` · `heapcheck` ·
`fuzz` · `selftest` — all scriptable through `shell_exec()`.

</td>
</tr>
</table>

## 🔬 KFUZZ: the kernel fuzzes itself

A bad input in ring 0 normally kills the machine. KFUZZ makes it a finding:

```mermaid
flowchart LR
    S[seed] --> R[xorshift64* PRNG]
    R --> T["target<br/>heap · pmm · string<br/>pthread · shell · netrx"]
    T -->|returns| O{"oracles<br/>heap walker · tree check<br/>bad-free counter"}
    T -->|"CPU fault #PF/#GP…"| X[exception handler]
    X -->|longjmp| C["crash report<br/>seed + vector + RIP<br/>fuzz replay ‹seed›"]
    O -->|new coverage| K[(corpus)]
    K --> S
```

- **Sandbox** — `setjmp`/`longjmp` written in assembly; a fault inside a
  target unwinds back to the harness instead of halting the kernel.
- **Coverage** — subject code is built with `-fsanitize-coverage=trace-pc`;
  seeds that reach new edges are kept and mutated.
- **Oracles** — after every run the heap, the process tree and the free
  counters must still be consistent.
- **Watchdog** — a target that hangs is reported with its seed.
- **Replay** — `fuzz replay <seed> <target>` reproduces any finding exactly.

## 🧱 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  shell (shell_exec)            KFUZZ (sandbox · coverage)     │
├──────────────────────────────────────────────────────────────┤
│  BSD sockets  ·  TCP / UDP / ICMP  ·  IPv4  ·  ARP  ·  Eth   │
│  netd thread + net_lock                                       │
├───────────────────────────┬──────────────────────────────────┤
│  pthreads · sem · errno   │  e1000 · PCI · loopback           │
├───────────────────────────┴──────────────────────────────────┤
│  preemptive scheduler · wait queues · process table/tree      │
├──────────────────────────────────────────────────────────────┤
│  PMM (bitmap) · VMM (paging) · kernel heap (+ integrity walk) │
├──────────────────────────────────────────────────────────────┤
│  GDT · IDT · PIC · PIT · TSS · debug registers  (x86-64)      │
└──────────────────────────────────────────────────────────────┘
```

<details>
<summary><b>Source layout</b></summary>

```
kernel/
  arch/        GDT, IDT, PIC, TSS, context switch, debug registers
  mm/          physical/virtual memory, kernel heap
  proc/        PCB, process table/tree, PIDs, scheduler
  pthread/     POSIX threads, mutexes, condvars, semaphores
  drivers/     PCI, e1000, timer, keyboard, serial
  net/         Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets
  shell/       the command shell
  kfuzz/       the ring-0 self-fuzzer
  tests/       in-kernel KTEST suites
tools/         test runner, keyboard-driven smoke test, stress runner
docs/          per-phase design documents
```

</details>

## 🚀 Quick start

You need `gcc`, `nasm`, `grub-mkrescue` (with `xorriso`) and
`qemu-system-x86_64`.

```sh
git clone https://github.com/medaminkh-dev/MAKH.git && cd MAKH
make            # build makhos.kernel + makhos.iso
make run        # boot it in QEMU
```

Type `help` at the `MakhOS>` prompt.

## ✅ Testing

| Command | What it does |
|---|---|
| `make test` | Boots a headless self-test image, runs every `KTEST` (incl. fuzz campaigns), exits pass/fail |
| `make smoke` | Boots the real image and types shell commands through the emulated keyboard |
| `make stress` | Runs the whole suite many times in parallel to flush out timing bugs |
| `make check-license` | Verifies every source file carries its SPDX license header |

CI runs the suite and a stress job on every push.

## 📈 Status

| Phase | Milestone | |
|---|---|---|
| 1–9 | Boot, VGA/serial, memory, interrupts, timer, keyboard, GDT/TSS, syscall MSRs, processes | ✅ |
| 11 | In-kernel test harness, `kprintf`, panic backtraces | ✅ |
| 12 | Preemptive priority scheduler, sleep, wait queues | ✅ |
| 13 | POSIX threads API | ✅ |
| 14 | Networking: PCI, e1000, TCP/IP, sockets, shell | ✅ [docs](docs/PHASE14_NETWORK.md) |
| 15 | KFUZZ ring-0 self-fuzzer | ✅ [docs](docs/PHASE15_KFUZZ.md) |
| 16 | Ring-3 userspace: syscall/sysret ABI, uaccess, fault containment | ✅ [docs](docs/PHASE16_USERSPACE.md) |

## 📚 Documentation

Design documents for each phase live in [`docs/`](docs/), including
[networking](docs/PHASE14_NETWORK.md) and [KFUZZ](docs/PHASE15_KFUZZ.md).

## ⚖️ License

MakhOS is **dual-licensed**:

- **[GNU AGPL-3.0](LICENSE)** — free for everyone who shares their
  modifications, including when running a modified MakhOS as a network
  service.
- **[Commercial license](LICENSING.md)** — for products, appliances and
  hosted services that need to keep their changes private, or want support
  and a warranty.

Earlier revisions were published under BSD-3-Clause; see [NOTICE](NOTICE).
"MakhOS", "MAKH" and "KFUZZ" are project names and are not licensed for use
by modified versions.

## 🤝 Contributing

Contributions are welcome — please read [CONTRIBUTING.md](CONTRIBUTING.md)
and sign the [CLA](CLA.md) in your first pull request.

## 📖 The story

The original `main` became unstable: features were piling up on a fragile
process foundation, and every fix created two new bugs. So we stopped,
stepped back to Phase 8 — the last solid point — and rebuilt one phase at a
time, with tests meaner than any user.

> Sometimes you have to go backward to move forward.

<div align="center">

**MakhOS** · © 2026 Amine Khemissi · AGPL-3.0-only or commercial

</div>
