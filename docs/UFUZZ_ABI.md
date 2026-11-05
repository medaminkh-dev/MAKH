<!-- GENERATED FILE — do not edit by hand.
     Source of truth: fuzz/abi/makh_syscalls.json
     Regenerate: python3 fuzz/abi/check_abi.py --emit-doc -->

# MAKH UFUZZ — ABI Inventory & Contract (U0)

ABI version **1**. Source of truth: `kernel/include/syscall.h (numbers) + kernel/syscall/syscall.c (dispatch)`.

This inventory is what UFUZZ treats as the MAKH Ring-3 syscall ABI. It is checked against the live kernel table (`kernel/include/syscall.h` numbers and the dispatched `case SYS_*:` labels in `kernel/syscall/syscall.c`) by `fuzz/abi/check_abi.py`, which is a CI gate. A syscall may not be listed as supported without a contract, every pointer argument carries a direction and a size rule, and every call states what resources it creates, consumes or releases.

**Totals:** 80 syscalls — 48 implemented, 25 partial, 7 stub.

## Legend

- **impl:** `implemented` full contract honoured for the supported argument domain; `partial` dispatched and useful, but ignores or narrows some arguments/flags (documented per call); `stub` dispatched but returns a constant / no-op; performs no real work; `unsupported` not dispatched; returns -ENOSYS (listed only when a program is likely to try it)
- **arg notation:** a pointer shows `(dir size)`; a scalar that names a resource shows `(resource)`.
- **dir:** `in` kernel reads · `out` kernel writes · `inout` both.
- **size rules:** `cstr` NUL-terminated · `len:ARG` length from ARG · `struct:NAME` fixed struct · `arr:ARG:ELEM` array · `fixed:N` N bytes · `cstr_arr` NULL-terminated string array.
- **resources:** `+x` creates · `~x` consumes · `-x` releases, where x ∈ {fd, pid, tid, addr, socket, pipe_end, path, buffer, signal_context}.

## Summary

| nr | name | impl | handler | args | resources |
|---:|------|------|---------|------|-----------|
| 0 | `read` | implemented | `do_read` | fd (fd), buf (out len:count), count | — |
| 1 | `write` | implemented | `do_write` | fd (fd), buf (in len:count), count | — |
| 2 | `open` | implemented | `do_open` | path (in cstr), flags | +fd |
| 3 | `close` | implemented | `vfs_close` | fd (fd) | -fd |
| 4 | `stat` | implemented | `do_lstat` | path (in cstr), statbuf (out struct:stat) | — |
| 5 | `fstat` | implemented | `do_fstat` | fd (fd), statbuf (out struct:stat) | — |
| 6 | `lstat` | implemented | `do_lstat` | path (in cstr), statbuf (out struct:stat) | — |
| 8 | `lseek` | implemented | `vfs_lseek` | fd (fd), offset, whence | — |
| 9 | `mmap` | partial | `do_mmap` | addr (addr), length, prot, flags | +addr |
| 10 | `mprotect` | implemented | `do_mprotect` | addr (addr), len, prot | — |
| 11 | `munmap` | implemented | `do_munmap` | addr (addr), len | -addr |
| 12 | `brk` | implemented | `do_brk` | addr | — |
| 13 | `rt_sigaction` | partial | `do_sigaction` | signum, handler (signal_context), restorer | — |
| 14 | `rt_sigprocmask` | implemented | `do_sigprocmask` | how, set, oldset (out fixed:8) | — |
| 15 | `rt_sigreturn` | implemented | `signal_sigreturn` | — | ~signal_context |
| 16 | `ioctl` | partial | `do_ioctl` | fd (fd), request, argp (inout struct:termios) | — |
| 19 | `readv` | implemented | `do_readv` | fd (fd), iov (in arr:iovcnt:iovec), iovcnt | — |
| 20 | `writev` | implemented | `do_writev` | fd (fd), iov (in arr:iovcnt:iovec), iovcnt | — |
| 21 | `access` | partial | `do_access` | path (in cstr), mode | — |
| 22 | `pipe` | implemented | `do_pipe` | pipefd (out fixed:8) | +pipe_end +pipe_end |
| 28 | `madvise` | stub | `(inline)` | addr (in len:length), length, advice | — |
| 33 | `dup2` | implemented | `do_dup2` | oldfd (fd), newfd (fd) | +fd |
| 35 | `nanosleep` | implemented | `do_nanosleep` | req (in struct:timespec), rem (out struct:timespec) | — |
| 39 | `getpid` | implemented | `do_getpid` | — | — |
| 41 | `socket` | partial | `do_socket` | domain, type, protocol | +fd +socket |
| 42 | `connect` | partial | `do_connect` | fd (socket), addr (in len:addrlen), addrlen | — |
| 54 | `setsockopt` | partial | `do_setsockopt` | fd (socket), level, optname, optval (in struct:timeval) | — |
| 56 | `clone` | partial | `proc_clone` | flags, stack (in fixed:0), parent_tid (out fixed:4), child_tid (out fixed:4), tls | +tid |
| 57 | `fork` | implemented | `proc_fork` | — | +pid |
| 59 | `execve` | implemented | `proc_execve` | path (in cstr), argv (in cstr_arr), envp (in cstr_arr) | -addr |
| 60 | `exit` | implemented | `thread_exit/usermode_exit` | status | -pid -fd -addr |
| 61 | `wait4` | partial | `do_wait4` | pid (pid), wstatus (out fixed:4) | ~pid |
| 62 | `kill` | implemented | `signal_kill` | pid (pid), sig (signal_context) | — |
| 63 | `uname` | implemented | `do_uname` | buf (out struct:utsname) | — |
| 72 | `fcntl` | partial | `do_fcntl` | fd (fd), cmd, arg | +fd |
| 79 | `getcwd` | implemented | `do_getcwd` | buf (out len:size), size | — |
| 80 | `chdir` | implemented | `do_chdir` | path (in cstr) | — |
| 82 | `rename` | implemented | `do_rename` | oldpath (in cstr), newpath (in cstr) | — |
| 83 | `mkdir` | implemented | `do_mkdir` | path (in cstr), mode | — |
| 84 | `rmdir` | implemented | `do_unlink` | path (in cstr) | — |
| 87 | `unlink` | implemented | `do_unlink` | path (in cstr) | — |
| 88 | `symlink` | implemented | `do_symlink` | target (in cstr), linkpath (in cstr) | — |
| 89 | `readlink` | implemented | `do_readlink` | path (in cstr), buf (out len:bufsiz), bufsiz | — |
| 90 | `chmod` | implemented | `do_chmod` | path (in cstr), mode | — |
| 96 | `gettimeofday` | implemented | `do_gettimeofday` | tv (out struct:timeval), tz | — |
| 97 | `getrlimit` | partial | `do_getrlimit` | resource, rlim (out struct:rlimit) | — |
| 102 | `getuid` | stub | `(inline)` | — | — |
| 104 | `getgid` | stub | `(inline)` | — | — |
| 107 | `geteuid` | stub | `(inline)` | — | — |
| 108 | `getegid` | stub | `(inline)` | — | — |
| 109 | `setpgid` | implemented | `sys_setpgid` | pid (pid), pgid | — |
| 110 | `getppid` | implemented | `do_getppid` | — | — |
| 111 | `getpgrp` | implemented | `sys_getpgid(0)` | — | — |
| 112 | `setsid` | implemented | `sys_setsid` | — | — |
| 121 | `getpgid` | implemented | `sys_getpgid` | pid (pid) | — |
| 158 | `arch_prctl` | implemented | `do_arch_prctl` | code, addr (inout fixed:8) | — |
| 160 | `setrlimit` | stub | `do_setrlimit` | resource, rlim (in struct:rlimit) | — |
| 202 | `futex` | partial | `do_futex` | uaddr (in fixed:4), op, val, timeout (in struct:timespec) | — |
| 204 | `sched_getaffinity` | partial | `do_sched_getaffinity` | pid, cpusetsize, mask (out len:cpusetsize) | — |
| 217 | `getdents64` | implemented | `do_getdents64` | fd (fd), dirp (out len:count), count | — |
| 218 | `set_tid_address` | implemented | `do_set_tid_address` | tidptr (out fixed:4) | — |
| 228 | `clock_gettime` | implemented | `do_clock_gettime` | clockid, tp (out struct:timespec) | — |
| 231 | `exit_group` | implemented | `thread_exit/usermode_exit` | status | -pid -fd -addr |
| 257 | `openat` | implemented | `do_openat` | dirfd (fd), path (in cstr), flags | +fd |
| 258 | `mkdirat` | partial | `do_mkdir` | dirfd (fd), path (in cstr), mode | — |
| 262 | `newfstatat` | partial | `do_newfstatat` | dirfd (fd), path (in cstr), statbuf (out struct:stat) | — |
| 263 | `unlinkat` | partial | `do_unlinkat` | dirfd (fd), path (in cstr) | — |
| 264 | `renameat` | partial | `do_rename` | olddirfd (fd), oldpath (in cstr), newdirfd (fd), newpath (in cstr) | — |
| 266 | `symlinkat` | partial | `do_symlink` | target (in cstr), newdirfd (fd), linkpath (in cstr) | — |
| 267 | `readlinkat` | partial | `do_readlink` | dirfd (fd), path (in cstr), buf (out len:bufsiz), bufsiz | — |
| 268 | `fchmodat` | partial | `do_chmod` | dirfd (fd), path (in cstr), mode | — |
| 269 | `faccessat` | partial | `do_access` | dirfd (fd), path (in cstr) | — |
| 293 | `pipe2` | partial | `do_pipe` | pipefd (out fixed:8), flags | +pipe_end +pipe_end |
| 302 | `prlimit64` | partial | `do_prlimit64` | pid (pid), resource, new_limit, old_limit (out struct:rlimit64) | — |
| 316 | `renameat2` | partial | `do_rename` | olddirfd (fd), oldpath (in cstr), newdirfd (fd), newpath (in cstr), flags | — |
| 318 | `getrandom` | partial | `do_getrandom` | buf (out len:buflen), buflen, flags | — |
| 512 | `makh_getticks` | implemented | `timer_get_ticks` | — | — |
| 513 | `makh_sleep_ms` | stub | `(inline)` | ms | — |
| 514 | `makh_ping` | implemented | `do_makh_ping` | ip, seq, timeout_ms | — |
| 515 | `makh_ifinfo` | implemented | `do_makh_ifinfo` | index, buf (out struct:makh_ifinfo) | — |

## Contracts

### `read` — SYS_READ (nr 0, implemented)

- **handler:** `do_read` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `buf` : `void*` — ptr out, size `len:count`
  - `count` : `size_t`
- **returns:** `ssize_t` — bytes read; 0 at EOF; -errno on error
- **errno:** `EBADF`, `EFAULT`, `EINVAL`
- **resources:** —
- **side effects:** advances the open-file offset
- **concurrency:** may block on a pipe/tty until data or EOF

### `write` — SYS_WRITE (nr 1, implemented)

- **handler:** `do_write` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `buf` : `const void*` — ptr in, size `len:count`
  - `count` : `size_t`
- **returns:** `ssize_t` — bytes written; -errno on error
- **errno:** `EBADF`, `EFAULT`, `EPIPE`
- **resources:** —
- **side effects:** advances the open-file offset; may raise SIGPIPE on a broken pipe

### `open` — SYS_OPEN (nr 2, implemented)

- **handler:** `do_open` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `flags` : `int`
- **returns:** `int` — new fd; -errno on error
- **errno:** `ENOENT`, `EFAULT`, `EMFILE`, `EEXIST`
- **resources:** +fd
- **flags:** `{"supported": ["O_CREAT", "O_TRUNC", "O_APPEND", "O_ACCMODE"], "note": "mode argument is not consumed"}`

### `close` — SYS_CLOSE (nr 3, implemented)

- **handler:** `vfs_close` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
- **returns:** `int` — 0 on success; -EBADF for a bad fd (fd 0/1/2 empty slot still succeeds)
- **errno:** `EBADF`
- **resources:** -fd
- **side effects:** last reference frees the open-file description; a pipe end detaches and may raise EOF/EPIPE on the peer

### `stat` — SYS_STAT (nr 4, implemented)

- **handler:** `do_lstat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `statbuf` : `struct stat*` — ptr out, size `struct:stat`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** stat and lstat share do_lstat; symlink-following distinction is not modelled

### `fstat` — SYS_FSTAT (nr 5, implemented)

- **handler:** `do_fstat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `statbuf` : `struct stat*` — ptr out, size `struct:stat`
- **returns:** `int` — 0; -errno
- **errno:** `EBADF`, `EFAULT`
- **resources:** —

### `lstat` — SYS_LSTAT (nr 6, implemented)

- **handler:** `do_lstat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `statbuf` : `struct stat*` — ptr out, size `struct:stat`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —

### `lseek` — SYS_LSEEK (nr 8, implemented)

- **handler:** `vfs_lseek` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `offset` : `off_t`
  - `whence` : `int`
- **returns:** `off_t` — resulting absolute offset; -errno
- **errno:** `EBADF`, `EINVAL`
- **resources:** —
- **flags:** `{"supported": ["SEEK_SET", "SEEK_CUR", "SEEK_END"]}`

### `mmap` — SYS_MMAP (nr 9, partial)

- **handler:** `do_mmap` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `addr` : `void*` — resource `addr` — hint only, ignored (bump allocator chooses the address). An address operand, not a data pointer: the kernel never copy_*_user's it
  - `length` : `size_t`
  - `prot` : `int`
  - `flags` : `int`
- **returns:** `void*` — mapped base address; MAP_FAILED(-1) on error
- **errno:** `ENOMEM`, `EINVAL`, `ENOSYS`
- **resources:** +addr
- **flags:** `{"supported": ["PROT_NONE", "PROT_READ", "PROT_WRITE", "PROT_EXEC", "MAP_PRIVATE", "MAP_ANONYMOUS"], "ignored": ["fd", "offset"]}`
- **notes:** anonymous private mappings only; no file-backed mappings; the addr hint is ignored

### `mprotect` — SYS_MPROTECT (nr 10, implemented)

- **handler:** `do_mprotect` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `addr` : `void*` — resource `addr` — page-aligned base of an existing mapping; an address/region operand into uvm_mprotect, not a data pointer (no copy_*_user)
  - `len` : `size_t`
  - `prot` : `int`
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`, `ENOMEM`, `EACCES`
- **resources:** —
- **flags:** `{"supported": ["PROT_NONE", "PROT_READ", "PROT_WRITE", "PROT_EXEC"]}`

### `munmap` — SYS_MUNMAP (nr 11, implemented)

- **handler:** `do_munmap` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `addr` : `void*` — resource `addr` — base of the mapping to release; an address/region operand into uvm_munmap, not a data pointer (no copy_*_user)
  - `len` : `size_t`
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`
- **resources:** -addr

### `brk` — SYS_BRK (nr 12, implemented)

- **handler:** `do_brk` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `addr` : `void*` — new program break, or 0 to query
- **returns:** `void*` — the resulting program break
- **errno:** —
- **resources:** —
- **side effects:** grows/shrinks the process heap region

### `rt_sigaction` — SYS_RT_SIGACTION (nr 13, partial)

- **handler:** `do_sigaction` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `signum` : `int`
  - `handler` : `void (*)(int)` — resource `signal_context` — a user CODE address stored verbatim as the disposition (SIG_DFL=0, SIG_IGN=1 special); not dereferenced as data
  - `restorer` : `void (*)(void)` — user trampoline address that invokes rt_sigreturn
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`
- **resources:** —
- **side effects:** installs a per-process signal disposition
- **notes:** SEMANTIC ABI (audited): MAKH's rt_sigaction is (sig, handler, restorer), NOT Linux's (sig, const struct sigaction*, struct sigaction*). do_sigaction() forwards handler/restorer BY VALUE to signal_sigaction(); no sigaction struct is read and there is no oldact out-param. Matches user/usys.h usignal(). See docs/UFUZZ_ABI_AUDIT.md.

### `rt_sigprocmask` — SYS_RT_SIGPROCMASK (nr 14, implemented)

- **handler:** `do_sigprocmask` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `how` : `int`
  - `set` : `unsigned long` — the new mask bits passed BY VALUE, not a sigset_t pointer; evidence: signal_procmask((int)how, set, &old)
  - `oldset` : `sigset_t*` — ptr out, size `fixed:8` — optional; the previous mask is copy_to_user'd here when non-NULL
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`, `EFAULT`
- **resources:** —
- **flags:** `{"supported": ["SIG_BLOCK", "SIG_UNBLOCK", "SIG_SETMASK"]}`
- **notes:** SEMANTIC ABI (audited): the mask argument is by-value, not a pointer (do_sigprocmask forwards `set` to signal_procmask by value). Only oldset is a real user pointer. See docs/UFUZZ_ABI_AUDIT.md.

### `rt_sigreturn` — SYS_RT_SIGRETURN (nr 15, implemented)

- **handler:** `signal_sigreturn` (`kernel/syscall/syscall.c (syscall_dispatch)`)
- **arguments:** none
- **returns:** `n/a` — does not return normally; restores the pre-signal trapframe
- **errno:** —
- **resources:** ~signal_context
- **concurrency:** consumes the signal frame placed on the user stack by signal delivery
- **notes:** needs the live trapframe; dispatched specially in syscall_dispatch(), covered by /bin/sigfuzz

### `ioctl` — SYS_IOCTL (nr 16, partial)

- **handler:** `do_ioctl` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `request` : `unsigned long`
  - `argp` : `void*` — ptr inout, size `struct:termios` — interpretation depends on request; TCGETS/TCSETS use struct termios, TIOCGWINSZ uses struct winsize
- **returns:** `int` — 0; -ENOTTY for non-std fds or unknown request
- **errno:** `ENOTTY`, `EFAULT`
- **resources:** —
- **notes:** only fd 0/1/2 (the TTY) are handled; a small set of termios/winsize requests

### `readv` — SYS_READV (nr 19, implemented)

- **handler:** `do_readv` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `iov` : `const struct iovec*` — ptr in, size `arr:iovcnt:iovec` — the iovec array is read; each iov_base buffer is written (out)
  - `iovcnt` : `int`
- **returns:** `ssize_t` — total bytes read; -errno
- **errno:** `EBADF`, `EFAULT`, `EINVAL`
- **resources:** —

### `writev` — SYS_WRITEV (nr 20, implemented)

- **handler:** `do_writev` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `iov` : `const struct iovec*` — ptr in, size `arr:iovcnt:iovec` — the iovec array and each iov_base buffer are read
  - `iovcnt` : `int`
- **returns:** `ssize_t` — total bytes written; -errno
- **errno:** `EBADF`, `EFAULT`, `EINVAL`
- **resources:** —

### `access` — SYS_ACCESS (nr 21, partial)

- **handler:** `do_access` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `mode` : `int` — not consumed; existence check only
- **returns:** `int` — 0 if the path resolves; -ENOENT otherwise
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** existence-only: the access mode (R_OK/W_OK/X_OK) is ignored

### `pipe` — SYS_PIPE (nr 22, implemented)

- **handler:** `do_pipe` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pipefd` : `int*` — ptr out, size `fixed:8` — two ints: [0]=read end, [1]=write end
- **returns:** `int` — 0; -errno
- **errno:** `EFAULT`, `EMFILE`
- **resources:** +pipe_end +pipe_end

### `madvise` — SYS_MADVISE (nr 28, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `addr` : `void*` — ptr in, size `len:length`
  - `length` : `size_t`
  - `advice` : `int`
- **returns:** `int` — always 0 (advisory no-op)
- **errno:** —
- **resources:** —
- **notes:** accepted and ignored; does not validate addr/length

### `dup2` — SYS_DUP2 (nr 33, implemented)

- **handler:** `do_dup2` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `oldfd` : `int` — resource `fd`
  - `newfd` : `int` — resource `fd`
- **returns:** `int` — newfd; -errno
- **errno:** `EBADF`
- **resources:** +fd
- **side effects:** closes newfd first if it was open

### `nanosleep` — SYS_NANOSLEEP (nr 35, implemented)

- **handler:** `do_nanosleep` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `req` : `const struct timespec*` — ptr in, size `struct:timespec`
  - `rem` : `struct timespec*` — ptr out, size `struct:timespec`
- **returns:** `int` — 0; -EINTR if a signal interrupts (remaining time written to rem)
- **errno:** `EINTR`, `EINVAL`, `EFAULT`
- **resources:** —
- **concurrency:** blocks; interruptible by a signal

### `getpid` — SYS_GETPID (nr 39, implemented)

- **handler:** `do_getpid` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `pid_t` — the caller's pid
- **errno:** —
- **resources:** —

### `socket` — SYS_SOCKET (nr 41, partial)

- **handler:** `do_socket` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `domain` : `int`
  - `type` : `int`
  - `protocol` : `int`
- **returns:** `int` — new socket fd; -errno
- **errno:** `EAFNOSUPPORT`, `EINVAL`, `EMFILE`
- **resources:** +fd +socket
- **notes:** limited address families/types (see net/socket.c)

### `connect` — SYS_CONNECT (nr 42, partial)

- **handler:** `do_connect` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `socket`
  - `addr` : `const struct sockaddr*` — ptr in, size `len:addrlen`
  - `addrlen` : `socklen_t`
- **returns:** `int` — 0; -errno
- **errno:** `ENOTSOCK`, `EFAULT`, `ECONNREFUSED`
- **resources:** —

### `setsockopt` — SYS_SETSOCKOPT (nr 54, partial)

- **handler:** `do_setsockopt` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `socket`
  - `level` : `int`
  - `optname` : `int`
  - `optval` : `const void*` — ptr in, size `struct:timeval` — length inferred from optname: timeval for SO_RCVTIMEO/SO_SNDTIMEO, int otherwise
- **returns:** `int` — 0; -errno
- **errno:** `ENOTSOCK`, `EFAULT`
- **resources:** —
- **notes:** only SO_RCVTIMEO/SO_SNDTIMEO are meaningful

### `clone` — SYS_CLONE (nr 56, partial)

- **handler:** `proc_clone` (`kernel/syscall/syscall.c (syscall_dispatch)`)
- **arguments:**
  - `flags` : `unsigned long`
  - `stack` : `void*` — ptr in, size `fixed:0` — child stack top
  - `parent_tid` : `int*` — ptr out, size `fixed:4`
  - `child_tid` : `int*` — ptr out, size `fixed:4`
  - `tls` : `unsigned long`
- **returns:** `pid_t` — child tid to parent, 0 to child; -errno
- **errno:** `ENOMEM`, `EINVAL`
- **resources:** +tid
- **concurrency:** creates a thread sharing the address space (CLONE_VM path used by musl)
- **notes:** needs the live trapframe; supports the musl posix_spawn/pthread clone shape

### `fork` — SYS_FORK (nr 57, implemented)

- **handler:** `proc_fork` (`kernel/syscall/syscall.c (syscall_dispatch)`)
- **arguments:** none
- **returns:** `pid_t` — child pid to parent, 0 to child; -errno
- **errno:** `ENOMEM`
- **resources:** +pid
- **concurrency:** needs the live trapframe; child gets a copied address space (COW)

### `execve` — SYS_EXECVE (nr 59, implemented)

- **handler:** `proc_execve` (`kernel/syscall/syscall.c (syscall_dispatch)`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `argv` : `char* const*` — ptr in, size `cstr_arr`
  - `envp` : `char* const*` — ptr in, size `cstr_arr`
- **returns:** `int` — does not return on success; -errno on failure
- **errno:** `ENOENT`, `ENOEXEC`, `EFAULT`
- **resources:** -addr
- **side effects:** replaces the image; needs the live trapframe to re-enter the new program

### `exit` — SYS_EXIT (nr 60, implemented)

- **handler:** `thread_exit/usermode_exit` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `status` : `int` — low 8 bits used
- **returns:** `n/a` — does not return; the process becomes a zombie
- **errno:** —
- **resources:** -pid -fd -addr
- **side effects:** closes fds, wakes a waiter, raises SIGCHLD to the parent

### `wait4` — SYS_WAIT4 (nr 61, partial)

- **handler:** `do_wait4` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — resource `pid`
  - `wstatus` : `int*` — ptr out, size `fixed:4`
- **returns:** `pid_t` — reaped child pid; -ECHILD; -EINTR if a signal interrupts the wait
- **errno:** `ECHILD`, `EINTR`, `EFAULT`
- **resources:** ~pid
- **concurrency:** blocks until a child exits; interruptible by a signal (T-P01)
- **notes:** options and rusage arguments are not consumed

### `kill` — SYS_KILL (nr 62, implemented)

- **handler:** `signal_kill` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — resource `pid`
  - `sig` : `int` — resource `signal_context`
- **returns:** `int` — 0; -errno
- **errno:** `ESRCH`, `EINVAL`
- **resources:** —
- **side effects:** sets a pending signal on the target; may wake a blocked target

### `uname` — SYS_UNAME (nr 63, implemented)

- **handler:** `do_uname` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `buf` : `struct utsname*` — ptr out, size `struct:utsname`
- **returns:** `int` — 0; -EFAULT
- **errno:** `EFAULT`
- **resources:** —

### `fcntl` — SYS_FCNTL (nr 72, partial)

- **handler:** `do_fcntl` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `cmd` : `int`
  - `arg` : `unsigned long` — interpretation depends on cmd
- **returns:** `int` — cmd-dependent; -errno
- **errno:** `EBADF`, `EINVAL`
- **resources:** +fd
- **notes:** a subset of commands (e.g. F_DUPFD / F_GETFD / F_SETFD / F_GETFL); creates an fd only for F_DUPFD

### `getcwd` — SYS_GETCWD (nr 79, implemented)

- **handler:** `do_getcwd` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `buf` : `char*` — ptr out, size `len:size`
  - `size` : `size_t`
- **returns:** `long` — length including NUL; -ERANGE if too small
- **errno:** `ERANGE`, `EFAULT`
- **resources:** —

### `chdir` — SYS_CHDIR (nr 80, implemented)

- **handler:** `do_chdir` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `ENOTDIR`, `EFAULT`
- **resources:** —
- **side effects:** changes the process working directory

### `rename` — SYS_RENAME (nr 82, implemented)

- **handler:** `do_rename` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `oldpath` : `const char*` — ptr in, size `cstr` — resource `path`
  - `newpath` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EXDEV`, `EFAULT`
- **resources:** —

### `mkdir` — SYS_MKDIR (nr 83, implemented)

- **handler:** `do_mkdir` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `mode` : `mode_t`
- **returns:** `int` — 0; -errno
- **errno:** `EEXIST`, `ENOENT`, `EFAULT`
- **resources:** —

### `rmdir` — SYS_RMDIR (nr 84, implemented)

- **handler:** `do_unlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `ENOTEMPTY`, `ENOTDIR`
- **resources:** —
- **notes:** ext2 unlink also removes empty directories, so rmdir maps to do_unlink

### `unlink` — SYS_UNLINK (nr 87, implemented)

- **handler:** `do_unlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EISDIR`, `EFAULT`
- **resources:** —

### `symlink` — SYS_SYMLINK (nr 88, implemented)

- **handler:** `do_symlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `target` : `const char*` — ptr in, size `cstr`
  - `linkpath` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `EEXIST`, `ENOENT`, `EFAULT`
- **resources:** —

### `readlink` — SYS_READLINK (nr 89, implemented)

- **handler:** `do_readlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `buf` : `char*` — ptr out, size `len:bufsiz`
  - `bufsiz` : `size_t`
- **returns:** `ssize_t` — bytes placed in buf (no NUL); -errno
- **errno:** `EINVAL`, `ENOENT`, `EFAULT`
- **resources:** —

### `chmod` — SYS_CHMOD (nr 90, implemented)

- **handler:** `do_chmod` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `mode` : `mode_t`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **side effects:** persists the new permission bits to the inode

### `gettimeofday` — SYS_GETTIMEOFDAY (nr 96, implemented)

- **handler:** `do_gettimeofday` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `tv` : `struct timeval*` — ptr out, size `struct:timeval`
  - `tz` : `void*` — timezone operand; NOT accessed by the implementation (never copy_*_user'd), so modelled as an ignored scalar rather than an out pointer
- **returns:** `int` — 0; -EFAULT
- **errno:** `EFAULT`
- **resources:** —

### `getrlimit` — SYS_GETRLIMIT (nr 97, partial)

- **handler:** `do_getrlimit` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `resource` : `int`
  - `rlim` : `struct rlimit*` — ptr out, size `struct:rlimit`
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`, `EFAULT`
- **resources:** —
- **notes:** returns fixed/approximate limits

### `getuid` — SYS_GETUID (nr 102, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `uid_t` — always 0 (single-user)
- **errno:** —
- **resources:** —

### `getgid` — SYS_GETGID (nr 104, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `gid_t` — always 0
- **errno:** —
- **resources:** —

### `geteuid` — SYS_GETEUID (nr 107, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `uid_t` — always 0
- **errno:** —
- **resources:** —

### `getegid` — SYS_GETEGID (nr 108, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `gid_t` — always 0
- **errno:** —
- **resources:** —

### `setpgid` — SYS_SETPGID (nr 109, implemented)

- **handler:** `sys_setpgid` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — resource `pid`
  - `pgid` : `pid_t`
- **returns:** `int` — 0; -errno
- **errno:** `ESRCH`, `EINVAL`
- **resources:** —

### `getppid` — SYS_GETPPID (nr 110, implemented)

- **handler:** `do_getppid` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `pid_t` — the parent pid
- **errno:** —
- **resources:** —

### `getpgrp` — SYS_GETPGRP (nr 111, implemented)

- **handler:** `sys_getpgid(0)` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `pid_t` — the caller's process group id
- **errno:** —
- **resources:** —

### `setsid` — SYS_SETSID (nr 112, implemented)

- **handler:** `sys_setsid` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `pid_t` — the new session id; -errno
- **errno:** `EPERM`
- **resources:** —
- **side effects:** starts a new session/process group

### `getpgid` — SYS_GETPGID (nr 121, implemented)

- **handler:** `sys_getpgid` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — resource `pid`
- **returns:** `pid_t` — the target's process group id; -errno
- **errno:** `ESRCH`
- **resources:** —

### `arch_prctl` — SYS_ARCH_PRCTL (nr 158, implemented)

- **handler:** `do_arch_prctl` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `code` : `int`
  - `addr` : `unsigned long` — ptr inout, size `fixed:8` — ARCH_SET_FS: addr is the value; ARCH_GET_FS: addr is an out pointer
- **returns:** `int` — 0; -EINVAL
- **errno:** `EINVAL`, `ENOSYS`
- **resources:** —
- **flags:** `{"supported": ["ARCH_SET_FS", "ARCH_GET_FS"]}`

### `setrlimit` — SYS_SETRLIMIT (nr 160, stub)

- **handler:** `do_setrlimit` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `resource` : `int`
  - `rlim` : `const struct rlimit*` — ptr in, size `struct:rlimit`
- **returns:** `int` — always 0 (no-op)
- **errno:** —
- **resources:** —
- **notes:** accepted and ignored

### `futex` — SYS_FUTEX (nr 202, partial)

- **handler:** `do_futex` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `uaddr` : `int*` — ptr in, size `fixed:4` — FUTEX_WAIT reads the 4-byte word to compare against val; FUTEX_WAKE does not read it (audited: not written by either)
  - `op` : `int`
  - `val` : `int`
  - `timeout` : `const struct timespec*` — ptr in, size `struct:timespec` — read only on FUTEX_WAIT when non-NULL
- **returns:** `int` — op-dependent; -EAGAIN mismatch; -ETIMEDOUT; -EINTR
- **errno:** `EAGAIN`, `ETIMEDOUT`, `EINTR`, `EINVAL`, `EFAULT`
- **resources:** —
- **flags:** `{"supported": ["FUTEX_WAIT", "FUTEX_WAKE"]}`
- **concurrency:** FUTEX_WAIT blocks; FUTEX_WAKE wakes; PRIVATE/CLOCK flags stripped

### `sched_getaffinity` — SYS_SCHED_GETAFFINITY (nr 204, partial)

- **handler:** `do_sched_getaffinity` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — ignored
  - `cpusetsize` : `size_t`
  - `mask` : `unsigned long*` — ptr out, size `len:cpusetsize`
- **returns:** `int` — bytes written; -EINVAL if too small
- **errno:** `EINVAL`, `EFAULT`
- **resources:** —
- **notes:** single CPU: always reports CPU 0 online

### `getdents64` — SYS_GETDENTS64 (nr 217, implemented)

- **handler:** `do_getdents64` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `fd` : `int` — resource `fd`
  - `dirp` : `void*` — ptr out, size `len:count`
  - `count` : `size_t`
- **returns:** `ssize_t` — bytes of dirents; 0 at end; -errno
- **errno:** `EBADF`, `EINVAL`, `EFAULT`
- **resources:** —

### `set_tid_address` — SYS_SET_TID_ADDRESS (nr 218, implemented)

- **handler:** `do_set_tid_address` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `tidptr` : `int*` — ptr out, size `fixed:4` — stored; cleared and futex-woken at thread exit
- **returns:** `pid_t` — the caller's tid
- **errno:** —
- **resources:** —

### `clock_gettime` — SYS_CLOCK_GETTIME (nr 228, implemented)

- **handler:** `do_clock_gettime` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `clockid` : `clockid_t`
  - `tp` : `struct timespec*` — ptr out, size `struct:timespec`
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`, `EFAULT`
- **resources:** —
- **flags:** `{"supported": ["CLOCK_REALTIME", "CLOCK_MONOTONIC"]}`

### `exit_group` — SYS_EXIT_GROUP (nr 231, implemented)

- **handler:** `thread_exit/usermode_exit` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `status` : `int`
- **returns:** `n/a` — does not return
- **errno:** —
- **resources:** -pid -fd -addr
- **notes:** no thread groups: identical to exit

### `openat` — SYS_OPENAT (nr 257, implemented)

- **handler:** `do_openat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd` — AT_FDCWD(-100) means the cwd
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `flags` : `int`
- **returns:** `int` — new fd; -errno
- **errno:** `ENOENT`, `EFAULT`, `EMFILE`
- **resources:** +fd

### `mkdirat` — SYS_MKDIRAT (nr 258, partial)

- **handler:** `do_mkdir` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd` — ignored; path treated as absolute/cwd-relative
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `mode` : `mode_t`
- **returns:** `int` — 0; -errno
- **errno:** `EEXIST`, `ENOENT`
- **resources:** —
- **notes:** dirfd not honoured (delegates to do_mkdir on the path)

### `newfstatat` — SYS_NEWFSTATAT (nr 262, partial)

- **handler:** `do_newfstatat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd`
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `statbuf` : `struct stat*` — ptr out, size `struct:stat`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** AT_* flags argument is not consumed

### `unlinkat` — SYS_UNLINKAT (nr 263, partial)

- **handler:** `do_unlinkat` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd`
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EISDIR`
- **resources:** —
- **notes:** AT_REMOVEDIR flag not consumed

### `renameat` — SYS_RENAMEAT (nr 264, partial)

- **handler:** `do_rename` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `olddirfd` : `int` — resource `fd`
  - `oldpath` : `const char*` — ptr in, size `cstr` — resource `path`
  - `newdirfd` : `int` — resource `fd`
  - `newpath` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** dirfds not honoured

### `symlinkat` — SYS_SYMLINKAT (nr 266, partial)

- **handler:** `do_symlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `target` : `const char*` — ptr in, size `cstr`
  - `newdirfd` : `int` — resource `fd`
  - `linkpath` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0; -errno
- **errno:** `EEXIST`, `ENOENT`
- **resources:** —
- **notes:** newdirfd not honoured

### `readlinkat` — SYS_READLINKAT (nr 267, partial)

- **handler:** `do_readlink` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd`
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `buf` : `char*` — ptr out, size `len:bufsiz`
  - `bufsiz` : `size_t`
- **returns:** `ssize_t` — bytes placed in buf; -errno
- **errno:** `EINVAL`, `ENOENT`
- **resources:** —
- **notes:** dirfd not honoured

### `fchmodat` — SYS_FCHMODAT (nr 268, partial)

- **handler:** `do_chmod` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd`
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
  - `mode` : `mode_t`
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** dirfd and flags not honoured

### `faccessat` — SYS_FACCESSAT (nr 269, partial)

- **handler:** `do_access` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `dirfd` : `int` — resource `fd`
  - `path` : `const char*` — ptr in, size `cstr` — resource `path`
- **returns:** `int` — 0 if the path resolves; -ENOENT
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** existence-only; dirfd and mode not honoured

### `pipe2` — SYS_PIPE2 (nr 293, partial)

- **handler:** `do_pipe` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pipefd` : `int*` — ptr out, size `fixed:8`
  - `flags` : `int` — ignored
- **returns:** `int` — 0; -errno
- **errno:** `EFAULT`, `EMFILE`
- **resources:** +pipe_end +pipe_end
- **notes:** O_CLOEXEC/O_NONBLOCK flags not honoured

### `prlimit64` — SYS_PRLIMIT64 (nr 302, partial)

- **handler:** `do_prlimit64` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `pid` : `pid_t` — resource `pid` — ignored (self only)
  - `resource` : `int`
  - `new_limit` : `const struct rlimit64*` — NOT accessed by the implementation (do_prlimit64 never copy_from_user's it), so modelled as an ignored operand rather than an in pointer
  - `old_limit` : `struct rlimit64*` — ptr out, size `struct:rlimit64` — optional; the fixed limit is copy_to_user'd here when non-NULL
- **returns:** `int` — 0; -errno
- **errno:** `EINVAL`, `EFAULT`
- **resources:** —
- **notes:** SEMANTIC ABI (audited): reports fixed limits; new_limit is accepted but never read or enforced. See docs/UFUZZ_ABI_AUDIT.md.

### `renameat2` — SYS_RENAMEAT2 (nr 316, partial)

- **handler:** `do_rename` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `olddirfd` : `int` — resource `fd`
  - `oldpath` : `const char*` — ptr in, size `cstr` — resource `path`
  - `newdirfd` : `int` — resource `fd`
  - `newpath` : `const char*` — ptr in, size `cstr` — resource `path`
  - `flags` : `unsigned int` — ignored
- **returns:** `int` — 0; -errno
- **errno:** `ENOENT`, `EFAULT`
- **resources:** —
- **notes:** dirfds and flags (RENAME_NOREPLACE/EXCHANGE) not honoured

### `getrandom` — SYS_GETRANDOM (nr 318, partial)

- **handler:** `do_getrandom` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `buf` : `void*` — ptr out, size `len:buflen`
  - `buflen` : `size_t`
  - `flags` : `unsigned int` — ignored
- **returns:** `ssize_t` — bytes written; -errno
- **errno:** `EFAULT`
- **resources:** —
- **notes:** never blocks; GRND_* flags ignored

### `makh_getticks` — SYS_MAKH_GETTICKS (nr 512, implemented)

- **handler:** `timer_get_ticks` (`kernel/syscall/syscall.c`)
- **arguments:** none
- **returns:** `int64_t` — timer ticks since boot
- **errno:** —
- **resources:** —
- **notes:** MAKH-specific (0x200); above the Linux range

### `makh_sleep_ms` — SYS_MAKH_SLEEP_MS (nr 513, stub)

- **handler:** `(inline)` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `ms` : `uint32_t`
- **returns:** `int` — always 0 (no-op from the ring-3 brick)
- **errno:** —
- **resources:** —
- **notes:** MAKH-specific (0x201); deliberately a no-op from ring 3

### `makh_ping` — SYS_MAKH_PING (nr 514, implemented)

- **handler:** `do_makh_ping` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `ip` : `uint32_t`
  - `seq` : `uint16_t`
  - `timeout_ms` : `uint32_t`
- **returns:** `int` — (ttl<<16)|rtt_ms on reply; -errno
- **errno:** `ETIMEDOUT`, `EHOSTUNREACH`
- **resources:** —
- **notes:** MAKH-specific (0x202); one ICMP echo

### `makh_ifinfo` — SYS_MAKH_IFINFO (nr 515, implemented)

- **handler:** `do_makh_ifinfo` (`kernel/syscall/syscall.c`)
- **arguments:**
  - `index` : `int`
  - `buf` : `struct makh_ifinfo*` — ptr out, size `struct:makh_ifinfo`
- **returns:** `int` — 0; -errno if no such interface
- **errno:** `ENODEV`, `EFAULT`
- **resources:** —
- **notes:** MAKH-specific (0x203)

