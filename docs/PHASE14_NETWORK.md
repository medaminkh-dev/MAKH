# MakhOS Phase 14: Networking — PCI, e1000, TCP/IP, BSD Sockets, Shell

**Version:** 0.1.0-dev
**Status:** COMPLETE — 71 in-kernel tests + interactive smoke test, 24/24 stress runs clean
**Depends on:** Phase 12 (preemptive scheduler), Phase 13 (pthreads, semaphores, errno)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture](#2-architecture)
3. [Hardware Layer: PCI + e1000](#3-hardware-layer-pci--e1000)
4. [The netd Thread and the Locking Model](#4-the-netd-thread-and-the-locking-model)
5. [Protocols](#5-protocols)
6. [BSD Socket API](#6-bsd-socket-api)
7. [The Shell](#7-the-shell)
8. [Debugging Tools Added](#8-debugging-tools-added)
9. [Critical Bug Found: init's Children Use-After-Free](#9-critical-bug-found-inits-children-use-after-free)
10. [Testing](#10-testing)
11. [Limits and Future Work](#11-limits-and-future-work)

---

## 1. Executive Summary

Phase 14 gives MakhOS a real network stack, from the PCI bus up to a POSIX
socket API, plus a command shell to drive it.

| Layer | What exists |
|---|---|
| Bus | PCI configuration mechanism #1, full bus scan, BAR decoding, bus mastering |
| NIC | Intel e1000 (82540EM, QEMU's default): DMA rings, IRQ-driven receive |
| Loopback | `lo` device with **deterministic packet-loss injection** for tests |
| L2/L3 | Ethernet, ARP (cache, gleaning, timeouts), IPv4 (routing, checksums) |
| L4 | ICMP echo, UDP, **TCP** (full RFC 793 state machine, retransmission, fast retransmit, flow control) |
| API | `socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/setsockopt/getsockname/shutdown` |
| Tools | `ifconfig`, `arp`, `ping`, `netstat`, `ps`, `mem`, `heapcheck`, `selftest` shell commands |

Measured under QEMU TCG:

- 200 KB over loopback TCP: ~20 ms
- 64 KB over loopback TCP with **1 in 7 frames dropped**: ~1.8 s, 12 retransmissions, byte-exact
- 4/4 ICMP echo replies from the QEMU slirp gateway (10.0.2.2) through the e1000

---

## 2. Architecture

```
  application threads (pthreads)                     kernel shell
        │  socket()/send()/recv()  ...                   │ ping / arp / netstat
        ▼                                                ▼
 ┌──────────────────────── net_lock (pthread mutex) ─────────────────────────┐
 │  socket.c   ─── fd table (64 slots), SO_RCVTIMEO/SO_SNDTIMEO, errno       │
 │  tcp.c      ─── tcb list, state machine, rings, timers                    │
 │  udp.c      ─── datagram queues                                           │
 │  icmp.c     ─── echo reply + icmp_ping()                                  │
 │  ipv4.c     ─── route, header, checksum, ARP resolution before build      │
 │  arp.c      ─── 32-entry cache, request/reply, gleaning                   │
 │  eth.c      ─── framing, dispatch by ethertype                            │
 └────────────────────────────────────────────────────────────────────────────┘
        ▲ poll()                               │ send()
        │                                      ▼
   ┌─────────┐   sem_post (IRQ-safe)    ┌──────────────┐
   │  netd   │ ◀─────────────────────── │ e1000 IRQ 11 │
   │ thread  │   every 50 ms: timers    └──────────────┘
   └─────────┘                          ┌──────────────┐
                                        │  lo (queue)  │
                                        └──────────────┘
```

**One lock, one worker.** Every piece of protocol state lives under a single
`net_lock`. Received frames are processed only by the `netd` thread; user
threads enter the stack through the socket API and block on per-socket
condition variables (`net_wait`), which atomically drop `net_lock` while
sleeping. On a uniprocessor kernel this is simpler and just as fast as fine-
grained locking, and it makes the stack trivially free of lock-order bugs.

---

## 3. Hardware Layer: PCI + e1000

### 3.1 PCI (`kernel/drivers/pci.c`)

- Configuration mechanism #1 (`0xCF8` address / `0xCFC` data), 8/16/32-bit reads.
- `pci_init()` scans every bus/slot/function once and caches vendor, device,
  class, BARs and the interrupt line.
- `pci_find(vendor, device)`, `pci_bar_address()`, `pci_enable_bus_mastering()`.

### 3.2 IRQ registration (`kernel/arch/idt.c`)

```c
int  irq_register(uint8_t irq, irq_fn_t fn, void* ctx);
void irq_unregister(uint8_t irq);
uint64_t irq_get_count(uint8_t irq);   /* per-line counter, used by tests */
```

Slave-PIC lines (8–15) also unmask the cascade line IRQ2, without which they
never reach the CPU.

### 3.3 MMIO mapping (`kernel/mm/vmm.c`)

`vmm_map_mmio(phys, size)` maps device registers uncached (`PCD|PWT`).

### 3.4 e1000 (`kernel/drivers/e1000.c`)

| Item | Value |
|---|---|
| RX / TX rings | 32 descriptors each, 16 B per descriptor |
| Buffers | 2 KB, taken from the PMM (identity-mapped, so virt == phys for DMA) |
| MAC | read from the EEPROM, programmed into RAL0/RAH0 with AV |
| RCTL | `EN | BAM | SECRC` (broadcast accept, strip CRC) |
| Interrupt | reads ICR, then `net_rx_notify()` — never touches the stack itself |

The IRQ handler does the minimum: acknowledge and wake `netd`. All parsing
happens in thread context under `net_lock`.

---

## 4. The netd Thread and the Locking Model

```c
static void* netd_main(void* arg) {
    for (;;) {
        sem_timedwait(&rx_sem, &next_tick);   /* IRQ or 50 ms tick */
        net_lock();
        for each device: dev->poll(dev);      /* frames -> net_input() */
        if (tick due) { arp_timer(); tcp_timer(); }
        net_unlock();
    }
}
```

Rules that keep this deadlock-free:

1. **IRQ handlers never take `net_lock`.** They only `sem_post` (non-blocking).
2. **netd never blocks on ARP.** If an outgoing packet from netd needs an
   unresolved MAC, `arp_resolve()` sends the request and returns `-EAGAIN`;
   TCP's retransmission timer re-sends later. User threads *do* wait (up to
   1 s), dropping `net_lock` while they do.
3. **ARP is resolved before the IP buffer is built.** `ipv4_send()` uses a
   static TX buffer; resolving first means a wait can never let another
   sender overwrite a half-built packet (this was a real bug found by the
   concurrent-client test).
4. **Device `send()` never takes `net_lock`** and never blocks for long.

---

## 5. Protocols

### 5.1 ARP
- 32-entry cache, 60 s TTL, eviction of the entry closest to expiry.
- **Gleaning**: every IPv4 packet received updates the sender's cache entry,
  so replies rarely need a fresh ARP round-trip.

### 5.2 IPv4
- Routing: loopback for 127/8, on-link for the device subnet, otherwise the
  default device's gateway. `ipv4_src_for()` picks the source address.
- Header checksum verified on input; DF set on output; no fragmentation
  (TCP MSS keeps segments below the MTU).

### 5.3 ICMP
- Echo reply for any echo request; `icmp_ping(dst, seq, timeout)` returns the
  RTT in ms or a negative errno.

### 5.4 UDP
- Per-socket datagram queue; `recvfrom` truncates to the buffer (like Linux,
  excess bytes are discarded); `SO_RCVTIMEO` → `EAGAIN`.

### 5.5 TCP

| Feature | Detail |
|---|---|
| State machine | All RFC 793 states incl. `CLOSING`, `TIME_WAIT`, `LAST_ACK` |
| Handshake | SYN with MSS option; listener backlog counts half-open + unaccepted |
| Buffers | 16 KB send ring + 16 KB receive ring per connection |
| Flow control | Advertised window = free receive space; zero-window probing |
| Retransmission | RTO starts at 300 ms, doubles to 3 s, 10 tries then `ETIMEDOUT` |
| Loss recovery | Go-back-N from `snd_una`; **fast retransmit on 3 duplicate ACKs** |
| Close | FIN queued after data; `shutdown(SHUT_WR)`; abortive RST if closed with unread data |
| TIME_WAIT | 500 ms (short on purpose: the kernel is its own peer in tests) |
| Orphans | Closed-but-unfinished connections linger up to 5 s, then are reaped. An orphan in `TIME_WAIT` releases its 32 KB of buffers on the next tick |

---

## 6. BSD Socket API

`#include <net/socket.h>` — POSIX signatures, `-1` + `errno` on failure
(per-thread `errno`, see Phase 13).

```c
int     socket(int domain, int type, int protocol);      /* AF_INET, SOCK_STREAM|SOCK_DGRAM */
int     bind(int fd, const struct sockaddr* addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr* addr, socklen_t* len);
int     connect(int fd, const struct sockaddr* addr, socklen_t len);
ssize_t send(int fd, const void* buf, size_t len, int flags);      /* MSG_DONTWAIT */
ssize_t recv(int fd, void* buf, size_t len, int flags);
ssize_t sendto(...);   ssize_t recvfrom(...);
int     setsockopt(int fd, int level, int optname, const void* v, socklen_t n);
int     getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int     shutdown(int fd, int how);
int     sock_close(int fd);   /* close(): there is no VFS yet */
```

Example — a thread-per-connection echo server:

```c
static void* worker(void* arg) {
    int fd = (int)(long)arg;
    char b[128];
    long n;
    while ((n = recv(fd, b, sizeof(b), 0)) > 0)
        send(fd, b, (size_t)n, 0);
    sock_close(fd);
    return NULL;
}

int l = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(7) };
bind(l, (struct sockaddr*)&sa, sizeof(sa));
listen(l, 8);
for (;;) {
    pthread_t t;
    int fd = accept(l, NULL, NULL);
    pthread_create(&t, NULL, worker, (void*)(long)fd);
    pthread_detach(t);
}
```

Closing a socket another thread is blocked on (`accept`/`recv`) wakes it with
`EBADF` — tested by `net.tcp_accept_timeout_and_close_unblocks`.

---

## 7. The Shell

`kernel/shell/shell.c` replaces the echo-only prompt. The whole shell is one
function, `int shell_exec(const char* line)`: copy, tokenize in place, look up
a static command table, run. No allocation, bounded line length (128), so it
is safe to drive with arbitrary bytes — the tests (and the Phase 15 fuzzer) do
exactly that.

```
MakhOS> ifconfig
lo: UP LOOPBACK
    inet 127.0.0.1  netmask 255.0.0.0
eth0: UP
    ether 52:54:00:12:34:56
    inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2
MakhOS> ping 10.0.2.2 3
PING 10.0.2.2: 3 echo requests
reply from 10.0.2.2: seq=1 time=0 ms
...
--- 10.0.2.2: 3 sent, 3 received, 0% loss
MakhOS> netstat
Proto Local                 Remote                State        Send-Q Recv-Q
tcp   127.0.0.1:49152       127.0.0.1:8004        TIME_WAIT         0      0  (orphan)
```

Commands: `help echo clear uptime mem heapcheck ps ifconfig arp netstat ping
selftest reboot`. Status codes follow the shell convention (`0` ok, `1` error,
`2` usage, `127` not found).

---

## 8. Debugging Tools Added

These were built to hunt the bug in §9 and stay as permanent infrastructure
(the self-fuzzer uses them as oracles).

| Tool | Where | What it does |
|---|---|---|
| **Stack canary** | `thread_create` / `schedule()` | `0x5AFEC0DE5AFEC0DE` at the lowest qword of every thread stack, checked on every context switch → `panic("stack overflow in thread …")` |
| **`kheap_check()`** | `mm/kheap.c` | Walks every block: magic, size, alignment, footer ↔ header, free-list membership, `heap_used` accounting. Returns 0 or reports the first bad block |
| **`proc_tree_check()`** | `proc/table/table.c` | Every child in every parent's list is a live table entry with the right `parent_pid`; `children_tail` and `child_count` match |
| **Hardware watchpoints** | `arch/debugreg.c` | `hw_watch_set(slot, addr, len, HWW_WRITE)` programs DR0–DR3/DR7; the CPU traps on the exact instruction that writes the address |
| **Loud exceptions** | `arch/idt.c` | Full register dump, current thread, frame-pointer backtrace from the faulting `RBP`, DR6 for watchpoints; under `makh.test` QEMU exits with status 3 instead of hanging |

---

## 9. Critical Bug Found: init's Children Use-After-Free

`net.tcp_concurrent_clients` (4 clients, 4 worker threads) hung
intermittently. A dump showed a worker's PCB starting with the bytes of the
TCP payload: `pid = 0x44434241` ("ABCD"), name `"NOPread"` (was `"pthread"`).

The hunt, step by step:

1. **Heap metadata intact** — `kheap_check()` on every context switch passed,
   so this was a stray *data* write into live memory, not a heap overrun.
2. **Watchpoint on the victim PCB** (`DR0 = &pcb->pid`) → trap in `netd`,
   inside `tcp_input`'s receive-ring copy: `t->rb[tail] = data[i]` where
   `t->rb` *equalled the PCB address*.
3. **Watchpoint on `&tcb->rb`** → trap in `proc_add_child()` called by
   `pthread_create()` in init: `parent->children_tail->sibling_next = child`.
   init's `children_tail` pointed at a PCB freed long ago, whose memory now
   held the TCB — so linking the new thread overwrote `tcb->rb` with the new
   PCB's address, and TCP then wrote payload into that PCB.
4. **Root cause**: init (and idle) were static PCBs that had never been
   inserted into the process table, so `proc_find(1)` returned NULL and
   `proc_remove_child()` silently skipped the unlink for **every child of
   init** since Phase 12. Each exited child stayed in init's list after being
   freed.

**Fix:** `proc_table_insert()` registers idle and init at boot;
`proc_remove_child()` now panics instead of silently skipping an unknown
parent; PIDs rotate (a cursor like Linux's `last_pid`) so a stale PID does not
immediately alias a new thread. Regression tests:
`sched.init_is_findable_and_tree_consistent`,
`sched.exiting_children_are_unlinked_and_orphans_reparented`.

Two smaller races found along the way were also fixed: the process table's
slot claim and the PID bitmap are now atomic (IRQs off), and the idle reaper
unlinks a thread with IRQs off.

---

## 10. Testing

### 10.1 In-kernel suites (`make test`)

| Suite | Tests | Highlights |
|---|---|---|
| `net` | 14 | checksum vectors, IP parsing, routing, ping, UDP ordering/timeouts/truncation, socket errno contract, TCP echo, 200 KB bulk with flow control, 64 KB **with 1/7 packet loss**, connection refused (RST, < 1 s), 4 concurrent clients (+ heap returns to baseline), accept timeout |
| `net_hw` | 3 | ARP resolves the slirp gateway, 4/4 pings over the e1000, NIC counters + IRQ count move for our own traffic |
| `shell` | 9 | tokenizer edge cases, 127 for unknown, 400-byte line, argument validation, every info command, ping over loopback |
| `sched` (+3) | | init findable, orphan reparenting, stack canary armed |
| `kheap` (+2) | | walker passes after random churn; detects a smashed footer and names the block |

### 10.2 Interactive smoke test (`make smoke`)

`tools/shell_smoke.py` boots the normal image, types commands through QEMU's
`sendkey` (i.e. through the real PS/2 driver) and checks each command's
output: `help`, `ifconfig`, `ping 10.0.2.2 3`, `arp`, `ping 127.0.0.1 2`,
`mem`, `ps`, `netstat`, and an unknown command.

### 10.3 Stress (`make stress`)

`tools/stress_tests.sh 24 4` runs the full suite 24 times in parallel QEMU
instances: **24/24 clean**.

---

## 11. Limits and Future Work

- No IP fragmentation/reassembly, no IP options, no TCP SACK/window scaling,
  no congestion control beyond go-back-N + fast retransmit.
- Single NIC route table (default device + gateway); no DHCP (static
  10.0.2.15/24, QEMU's slirp defaults).
- `sock_close()` instead of `close()` until a VFS gives sockets real file
  descriptors.
- Next: Phase 15 turns §8's oracles into a ring-0 **self-fuzzer** that
  attacks the heap, the scheduler, pthreads, the shell and the network RX
  parsers from a sandboxed thread.
