# UFUZZ U0 — Semantic ABI Audit

`fuzz/abi/check_abi.py` proves **mechanical** consistency: that every documented
syscall is dispatched, numbers match `kernel/include/syscall.h`, every pointer
argument carries a direction + size rule, and every call states its resource
ownership. It does **not**, by itself, prove that each argument's
kind/direction/size is *semantically* correct against the kernel implementation
— a wrong description that is internally consistent would pass.

This document records the **semantic** audit that closes that gap: each
argument was checked against the actual handler in `kernel/syscall/syscall.c`,
using the kernel's user-memory primitives as ground truth:

- `copy_from_user(dst, uptr, n)` ⇒ the kernel **reads** `uptr` ⇒ direction `in`,
  size `n`.
- `copy_to_user(uptr, src, n)` ⇒ the kernel **writes** `uptr` ⇒ direction `out`,
  size `n`.
- An argument forwarded **by value** to a helper (no `copy_*_user`, not
  dereferenced as data) is a **scalar**, even if its C type is a pointer.
- An address operand that names a memory region but is never copied (mmap /
  mprotect / munmap `addr`) is a **scalar** with `res: addr`, not a data pointer.

Audit date: against branch `claude/inspiring-hawking-pgrf59` (U0–U2).
Method: `grep -n "copy_from_user\|copy_to_user\|do_"` over `syscall.c`, then
read each handler body.

## Findings that corrected the inventory

The audit changed real entries (not just confirmed them). These are the
corrections, with evidence:

| syscall | was | corrected to | evidence (`kernel/syscall/syscall.c`) |
|---------|-----|--------------|----------------------------------------|
| `rt_sigaction` | `(signum, act:in struct sigaction*, oldact:out struct sigaction*)` | `(signum, handler:scalar code-addr, restorer:scalar code-addr)`; impl `implemented`→`partial` | `do_sigaction()` = `signal_sigaction((int)sig, handler, restorer)` — handler/restorer forwarded **by value**; no struct read, no oldact. Matches `user/usys.h` `usignal()`. |
| `rt_sigprocmask` | `set:in const sigset_t*` | `set:scalar` (mask bits by value) | `do_sigprocmask()` = `signal_procmask((int)how, set, &old)` — `set` passed **by value**; only `uoldset` is `copy_to_user`'d. |
| `mmap` `addr` | `ptr in fixed:0` | `scalar res:addr` | `do_mmap()` `(void)addr;` — hint ignored; no copy. |
| `mprotect` `addr` | `ptr inout len:len` | `scalar res:addr` | `do_mprotect()` → `uvm_mprotect(aspace, addr, len, prot)` — address operand, no copy. |
| `munmap` `addr` | `ptr in len:len` | `scalar res:addr` | `do_munmap()` → `uvm_munmap(aspace, addr, len)` — address operand, no copy. |
| `gettimeofday` `tz` | `ptr out fixed:8` | `scalar` (ignored) | `do_gettimeofday()` copies only `utv`; `utz` is never touched. |
| `prlimit64` `new_limit` | `ptr in struct:rlimit64` | `scalar` (ignored) | `do_prlimit64()` only `copy_to_user(uold)`; `unew` is never read. |
| `futex` `uaddr` | `ptr inout fixed:4` | `ptr in fixed:4` | `do_futex()`→`futex_wait` reads the word to compare with `val`; neither WAIT nor WAKE writes it. |

The `rt_sigaction` case is the headline: a mechanically-consistent but
semantically wrong description (Linux's struct-based ABI vs. MAKH's
handler/restorer ABI) that only an implementation audit catches.

## Confirmed-correct pointer arguments

Verified against the handler's `copy_*_user` calls; no change needed:

- **out** (`copy_to_user`): `read.buf`, `stat/lstat/fstat.statbuf` (`do_stat`/`do_lstat`/`do_fstat`),
  `uname.buf`, `getrlimit.rlim`, `prlimit64.old_limit`, `readlink.buf`,
  `getcwd.buf`, `wait4.wstatus`, `rt_sigprocmask.oldset`, `pipe/pipe2.pipefd`,
  `clock_gettime.tp`, `gettimeofday.tv`, `nanosleep.rem`, `getrandom.buf`,
  `sched_getaffinity.mask`, `makh_ifinfo.buf`, `getdents64.dirp`,
  `arch_prctl.addr` (ARCH_GET_FS branch).
- **in** (`copy_from_user`): `write.buf`, `writev/readv.iov`, every `path`
  (char-by-char `copy_from_user` in the `*_from_user` cstr copies),
  `nanosleep.req`, `futex.timeout`, `connect.addr`, `setsockopt.optval`,
  `ioctl.argp` (TCSETS), `execve.argv/envp` (read by `proc_execve`).
- **inout**: `ioctl.argp` overall (TCGETS writes, TCSETS reads);
  `arch_prctl.addr` overall (dual code/value role, noted on the entry).
- **deferred-write**: `set_tid_address.tidptr` (stored; written + futex-woken at
  thread exit, not during the call) — kept `out` with a note.

## What this audit does and does not establish

- **Does**: that every argument's kind/direction/size in
  `makh_syscalls.json` matches how the handler in `syscall.c` actually treats
  it, for the current branch.
- **Does not**: prove the handlers themselves are correct, cover semantics that
  live outside `syscall.c` beyond the delegations checked by hand
  (`proc_execve`, `signal_sigaction`, `futex_wait/wake`, `uvm_*`), or freeze the
  ABI — re-run this audit when a handler's user-memory behaviour changes.
  `check_abi.py` keeps the mechanical invariants from regressing; this document
  is the semantic layer and must be revisited by hand on ABI changes.
