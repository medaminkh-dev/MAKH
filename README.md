# MakhOS

A small x86_64 kernel, built brick by brick.

## The Story

We hit a wall around Phase 13 on `main`.

The `main` branch became unstable — not because any one piece was wrong, but
because we kept piling new features (syscalls, IPC, …) on top of a **fragile
process foundation**. Every fix created two new bugs. We were stuck in
technical debt.

So we stopped. We stepped back to Phase 8 — the last point before everything
broke — and rebuilt from a clean base, one phase at a time, with heavy tests at
every step instead of after the fact.

That base is this branch. Since then it has grown a real scheduler, a POSIX
threads API, a full network stack, and — the part we're proudest of — **a
kernel that fuzzes itself in a ring-0 sandbox**.

## Where it is now

| Phase | What landed | Docs |
|---|---|---|
| 11 | In-kernel test harness (`.ktests`), `kprintf`, panic + frame-pointer backtrace, MM fixes | — |
| 12 | Preemptive priority scheduler: run queues, sleep, wait queues, threads, idle reaper | — |
| 13 | POSIX threads: `pthread_*`, mutexes, condvars, semaphores, per-thread `errno`, `CLOCK_MONOTONIC` | — |
| 14 | Networking: PCI, e1000, ARP/IPv4/ICMP/UDP/**TCP**, BSD sockets, and a command shell | [PHASE14_NETWORK.md](docs/PHASE14_NETWORK.md) |
| 15 | **KFUZZ**: a ring-0, coverage-guided self-fuzzer with a fault-recovering sandbox | [PHASE15_KFUZZ.md](docs/PHASE15_KFUZZ.md) |

Highlights:

- **Preemptive multitasking** with priorities, sleep/wake, wait queues and
  clean thread teardown.
- **Clean POSIX threads**: `pthread_create/join/detach`, recursive/errorcheck
  mutexes, condition variables, semaphores with `sem_timedwait`, TLS keys.
- **A real TCP/IP stack**: e1000 driver over DMA, ARP, IPv4, ICMP, UDP and a
  full RFC 793 TCP (retransmission, fast retransmit, flow control, TIME_WAIT),
  behind a POSIX socket API. 200 KB over loopback in ~20 ms; 64 KB survives
  1-in-7 packet loss byte-exact.
- **The kernel fuzzes itself.** A sandboxed ring-0 thread throws random,
  seeded inputs at the heap, page allocator, string routines, pthreads API,
  shell parser and network receive path. CPU faults are caught and turned into
  reproducible crash reports; invariant oracles (heap walker, process-tree
  checker) run after every case.

## Build and run

Needs `gcc`, `nasm`, `grub-mkrescue` (+`xorriso`), and `qemu-system-x86_64`.

```sh
make            # build makhos.kernel and the bootable makhos.iso
make run        # boot it in QEMU
```

At the prompt, `help` lists the shell commands:

```
MakhOS> ifconfig            # interfaces + counters
MakhOS> ping 10.0.2.2 3     # ICMP echo through the e1000
MakhOS> netstat             # TCP connections + protocol stats
MakhOS> mem                 # heap/frame usage + integrity check
MakhOS> fuzz 5000           # fuzz the kernel for 5000 iterations
```

## Test

```sh
make test       # headless: run the in-kernel suite, exit with pass/fail
make smoke      # boot the real image and drive the shell over the keyboard
make stress     # run the whole suite 24x in parallel to flush timing bugs
```

`make test` boots a self-test image (`makh.test`) that runs every `KTEST`
(80+ tests, incl. the fuzzer campaigns) and powers QEMU off with a pass/fail
exit code — the same thing CI runs.

## Layout

```
kernel/
  arch/        GDT, IDT, PIC, TSS, context switch, debug registers
  mm/          PMM (bitmap), VMM (paging), kernel heap (+ integrity walker)
  proc/        PCB, process table/tree, PID, preemptive scheduler
  pthread/     POSIX threads, mutexes, condvars, semaphores
  net/         PCI/e1000, Ethernet, ARP, IPv4, ICMP, UDP, TCP, sockets
  shell/       the command shell (shell_exec)
  kfuzz/       the ring-0 self-fuzzer (sandbox, coverage, targets)
  tests/       in-kernel KTEST suites
tools/         run_tests.py, shell_smoke.py, stress_tests.sh
docs/          per-phase design docs
```

## The Lesson

Sometimes you have to go backward to move forward. And once you're moving,
the thing that keeps you moving is tests that are meaner than your users —
mean enough, now, that the kernel writes them for itself.
