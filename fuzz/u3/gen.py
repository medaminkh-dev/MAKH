# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U3 — deterministic typed-program generator (host-side, MVP).
#
# Builds structurally-valid, dependency-aware programs from the U0 ABI using the
# five MVP operand kinds. The generator never emits a use-before-create or
# use-after-release by itself; those are produced only by the explicit
# mutation helpers (tagged), so the validator can tell intended violations from
# bugs. Deterministic: the same seed yields the same program.

from u3 import (Program, Call, Abi, Lcg, IMM, RES, NIL, BUF, PATH, IOV)

# MVP syscall set: the resource-interesting calls plus a couple of leaves. All
# have only MVP-representable args (checked against the ABI at import by the
# selftest). Flag/length values are plausible; the host layer validates
# structure, the guest layer (U3 step 3) runs them.
O_RDWR, O_CREAT = 2, 0o100          # Linux x86_64 values MAKH mirrors
PROT_RW = 0x1 | 0x2
MAP_PRIV_ANON = 0x02 | 0x20


class Gen:
    def __init__(self, abi, seed):
        self.abi = abi
        self.r = Lcg(seed)
        self.p = Program(prng=("lcg", seed))
        self.live_fd = []       # handle ids
        self.live_addr = []     # handle ids
        self._next_buf = 0
        self._next_path = 0

    # -- pools --
    def new_handle(self, rtype):
        hid = len(self.p.handles)
        self.p.handles.append(rtype)
        return hid

    def new_buf(self, size, fill=True):
        bid = self._next_buf
        self._next_buf += 1
        data = self.r.bytes(size) if fill else bytes(size)
        self.p.buffers.append((bid, size, data))
        return bid

    def new_path(self):
        pid = self._next_path
        self._next_path += 1
        self.p.paths.append((pid, "/tmp/u3_%d" % pid))
        return pid

    # -- call builders (operands follow each syscall's ABI arg order) --
    def c_open(self):
        # Open an existing, readable file (present in every boot's initrd) so the
        # emitted program gets a real fd and reads return data instead of
        # blocking or erroring; O_RDONLY keeps it side-effect free.
        h = self.new_handle("fd")
        self.p.calls.append(Call("open",
                                 [(PATH, self._fixed_path("/bin/hello")), (IMM, 0)],
                                 bind=("fd", [h])))
        self.live_fd.append(h)

    def _fixed_path(self, s):
        for pid, ps in self.p.paths:
            if ps == s:
                return pid
        pid = self._next_path
        self._next_path += 1
        self.p.paths.append((pid, s))
        return pid

    def c_pipe(self):
        h0, h1 = self.new_handle("fd"), self.new_handle("fd")
        b = self.new_buf(8, fill=False)             # pipefd[2]
        self.p.calls.append(Call("pipe", [(BUF, b)], bind=("fd", [h0, h1])))
        self.live_fd += [h0, h1]

    def c_write(self):
        fd = self._pick(self.live_fd)
        n = 1 + self.r.below(64)
        b = self.new_buf(n)
        self.p.calls.append(Call("write", [(RES, fd), (BUF, b), (IMM, n)]))

    def c_read(self):
        fd = self._pick(self.live_fd)
        n = 1 + self.r.below(64)
        b = self.new_buf(n, fill=False)
        self.p.calls.append(Call("read", [(RES, fd), (BUF, b), (IMM, n)]))

    def c_lseek(self):
        fd = self._pick(self.live_fd)
        self.p.calls.append(Call("lseek", [(RES, fd), (IMM, self.r.below(128)), (IMM, 0)]))

    def c_close(self, fd=None):
        fd = fd if fd is not None else self._pick(self.live_fd)
        self.live_fd.remove(fd)
        self.p.calls.append(Call("close", [(RES, fd)]))

    def c_mmap(self):
        h = self.new_handle("addr")
        self.p.calls.append(Call("mmap",
                                 [(IMM, 0), (IMM, 4096), (IMM, PROT_RW), (IMM, MAP_PRIV_ANON)],
                                 bind=("addr", [h])))
        self.live_addr.append(h)

    def c_mprotect(self):
        a = self._pick(self.live_addr)
        self.p.calls.append(Call("mprotect", [(RES, a), (IMM, 4096), (IMM, 0x1)]))

    def c_munmap(self, a=None):
        a = a if a is not None else self._pick(self.live_addr)
        self.live_addr.remove(a)
        self.p.calls.append(Call("munmap", [(RES, a), (IMM, 4096)]))

    def c_getpid(self):
        self.p.calls.append(Call("getpid", []))

    def _pick(self, pool):
        return pool[self.r.below(len(pool))]

    def build(self, steps=8):
        # NOTE: `pipe` is intentionally not in the random mix — a generated
        # read on an empty pipe would block. pipe lives in the hand-crafted
        # round-trip template (emit.py), where a write precedes the read.
        for _ in range(steps):
            choices = ["open", "mmap", "getpid"]
            if self.live_fd:
                choices += ["write", "read", "lseek", "close"]
            if self.live_addr:
                choices += ["mprotect", "munmap"]
            getattr(self, "c_" + choices[self.r.below(len(choices))])()
        # Balance: release everything still live (fresh-worker reset relies on it).
        for fd in list(self.live_fd):
            self.c_close(fd)
        for a in list(self.live_addr):
            self.c_munmap(a)
        return self.p


def generate(abi, seed, steps=8):
    return Gen(abi, seed).build(steps)


# -- mutation helpers: the ONLY source of intended lifecycle violations --
def mutate_use_after_release(abi, seed=0):
    """A deliberately use-after-close program (tagged), for the validator's
    positive/negative test and as a U3 adversarial template."""
    p = Program(prng=("lcg", seed))
    h = 0
    p.handles = ["fd"]
    pid = 0
    p.paths = [(0, "/tmp/u3_uac")]
    b = 0
    p.buffers = [(0, 8, bytes(8))]
    p.calls = [
        Call("open", [(PATH, 0), (IMM, O_RDWR | O_CREAT)], bind=("fd", [h])),
        Call("close", [(RES, h)]),
        Call("write", [(RES, h), (BUF, b), (IMM, 8)], tag="after_release"),
    ]
    return p


def mutate_use_before_create(seed=0):
    p = Program(prng=("lcg", seed))
    p.handles = ["fd"]
    p.buffers = [(0, 8, bytes(8))]
    p.calls = [
        Call("write", [(RES, 0), (BUF, 0), (IMM, 8)], tag="before_create"),
    ]
    return p


# ---- U3.1: variant operands (ioctl / arch_prctl) -------------------------
# The variant argp is still built from the five MVP operand kinds (an IMM
# selector + a BUF/IMM argp) — what U3.1 adds is making the generator choose a
# selector and a MATCHING argp shape, and the validator resolve them. No new
# guest operand is needed; the guest already passes a BUF as its address.
def _sel(abi, sysname, argname, casekey):
    """The numeric selector value for one variant case, from the ABI."""
    for a in abi.sc(sysname)["args"]:
        if a.get("name") == argname and a.get("kind") == "variant":
            return a["variants"][casekey]["sel_value"]
    raise KeyError("%s.%s[%s]" % (sysname, argname, casekey))


def variant_probe(abi):
    """A structurally-valid program that exercises every variant case the ABI
    models, so the validator's variant resolution is covered end to end.
    Side-effect-neutral: each mutating ioctl (TCSETS/TIOCSPGRP) and the FS base
    are set from values the program itself read or to a benign canonical one."""
    p = Program(prng=("lcg", 0))
    tsz = abi.struct_defs["termios"]["size"]
    # buffers: 0=termios, 1=pgid(4), 2=fsbase(8)
    p.buffers = [(0, tsz, bytes(tsz)), (1, 4, bytes(4)), (2, 8, bytes(8))]
    p.calls = [
        Call("ioctl", [(IMM, 1), (IMM, _sel(abi, "ioctl", "argp", "TCGETS")), (BUF, 0)]),
        Call("ioctl", [(IMM, 1), (IMM, _sel(abi, "ioctl", "argp", "TCSETS")), (BUF, 0)]),
        Call("ioctl", [(IMM, 1), (IMM, _sel(abi, "ioctl", "argp", "TIOCGPGRP")), (BUF, 1)]),
        Call("ioctl", [(IMM, 1), (IMM, _sel(abi, "ioctl", "argp", "TIOCSPGRP")), (BUF, 1)]),
        # ARCH_SET_FS takes the base BY VALUE (scalar) — a canonical user-half
        # address; ARCH_GET_FS writes the current base to an 8-byte out buffer.
        Call("arch_prctl", [(IMM, _sel(abi, "arch_prctl", "addr", "ARCH_SET_FS")), (IMM, 0x12345000)]),
        Call("arch_prctl", [(IMM, _sel(abi, "arch_prctl", "addr", "ARCH_GET_FS")), (BUF, 2)]),
    ]
    return p


def mutate_variant_bad_argp(abi):
    """Fail-first for variant resolution: ioctl(request=TCGETS) whose argp buffer
    is smaller than struct termios. The validator must reject it (the selected
    variant is an out ptr to `tsz` bytes), proving it resolves the selector
    rather than waving any buffer through for a `variant` arg."""
    p = Program(prng=("lcg", 0))
    p.buffers = [(0, 4, bytes(4))]          # 4 < sizeof(termios)
    p.calls = [
        Call("ioctl", [(IMM, 1), (IMM, _sel(abi, "ioctl", "argp", "TCGETS")), (BUF, 0)]),
    ]
    return p


# ---- U3.1b: iovec operands (readv / writev) ------------------------------
# An `iov` operand references a program-level iovec spec; the guest builds the
# struct iovec[] at runtime so each iov_base is a real buffer address.
def iov_probe():
    """A structurally-valid writev/readv-over-a-pipe program exercising the iov
    operand and the validator's array handling (gather of 2, scatter of 2)."""
    p = Program(prng=("lcg", 0))
    # 0:pipefd[2]; 1,2:writev sources; 3,4:readv dests
    p.buffers = [(0, 8, bytes(8)), (1, 2, b"ab"), (2, 3, b"cde"),
                 (3, 2, bytes(2)), (4, 3, bytes(3))]
    p.iovs = [[(1, 2), (2, 3)], [(3, 2), (4, 3)]]       # id 0 = write, id 1 = read
    p.handles = ["fd", "fd"]
    p.calls = [
        Call("pipe",   [(BUF, 0)], bind=("fd", [0, 1])),
        Call("writev", [(RES, 1), (IOV, 0), (IMM, 2)]),
        Call("readv",  [(RES, 0), (IOV, 1), (IMM, 2)]),
        Call("close",  [(RES, 0)]),
        Call("close",  [(RES, 1)]),
    ]
    return p


def mutate_iov_bad_len():
    """Fail-first: an iov segment whose buffer is smaller than its declared len.
    The validator must reject it (it checks each segment against its buffer),
    proving it inspects the iovec array rather than passing any iov through."""
    p = Program(prng=("lcg", 0))
    p.buffers = [(0, 8, bytes(8)), (1, 2, b"ab")]
    p.iovs = [[(1, 99)]]                                # 99 > buf 1's 2 bytes
    p.handles = ["fd", "fd"]
    p.calls = [
        Call("pipe",   [(BUF, 0)], bind=("fd", [0, 1])),
        Call("writev", [(RES, 1), (IOV, 0), (IMM, 1)]),
    ]
    return p


def mutate_iov_bad_count():
    """Fail-first: the iovcnt operand disagrees with the spec's segment count."""
    p = Program(prng=("lcg", 0))
    p.buffers = [(0, 8, bytes(8)), (1, 2, b"ab"), (2, 3, b"cde")]
    p.iovs = [[(1, 2), (2, 3)]]                         # 2 segments ...
    p.handles = ["fd", "fd"]
    p.calls = [
        Call("pipe",   [(BUF, 0)], bind=("fd", [0, 1])),
        Call("writev", [(RES, 1), (IOV, 0), (IMM, 3)]),  # ... but iovcnt says 3
    ]
    return p


# ---- U3.1c: hardening fail-firsts (negative ids/lens, kind & res-type) ----
def mutate_iov_negative_id():
    """Fail-first: a negative iov id (would index the iov list from the end)."""
    p = Program(prng=("lcg", 0))
    p.buffers = [(0, 8, bytes(8)), (1, 2, b"ab")]
    p.iovs = [[(1, 2)]]
    p.handles = ["fd", "fd"]
    p.calls = [
        Call("pipe",   [(BUF, 0)], bind=("fd", [0, 1])),
        Call("writev", [(RES, 1), (IOV, -1), (IMM, 1)]),   # negative iov id
    ]
    return p


def mutate_iov_negative_len():
    """Fail-first: an iov segment with a negative length (huge as a size_t)."""
    p = Program(prng=("lcg", 0))
    p.buffers = [(0, 8, bytes(8)), (1, 2, b"ab")]
    p.iovs = [[(1, -4)]]
    p.handles = ["fd", "fd"]
    p.calls = [
        Call("pipe",   [(BUF, 0)], bind=("fd", [0, 1])),
        Call("writev", [(RES, 1), (IOV, 0), (IMM, 1)]),
    ]
    return p


def mutate_res_type_mismatch():
    """Fail-first: an addr handle passed where an fd is wanted (close)."""
    p = Program(prng=("lcg", 0))
    p.handles = ["addr"]
    p.calls = [
        Call("mmap",  [(IMM, 0), (IMM, 4096), (IMM, PROT_RW), (IMM, MAP_PRIV_ANON)], bind=("addr", [0])),
        Call("close", [(RES, 0)]),          # close wants res:fd, handle 0 is an addr
    ]
    return p


def mutate_imm_for_ptr():
    """Fail-first: a raw immediate where the ABI wants a pointer (read's buf)."""
    p = Program(prng=("lcg", 0))
    p.handles = ["fd"]
    p.paths = [(0, "/bin/hello")]
    p.calls = [
        Call("open", [(PATH, 0), (IMM, 0)], bind=("fd", [0])),
        Call("read", [(RES, 0), (IMM, 1234), (IMM, 8)]),   # IMM where buf (ptr) expected
    ]
    return p
