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

from u3 import (Program, Call, Abi, Lcg, IMM, RES, NIL, BUF, PATH)

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
        h = self.new_handle("fd")
        self.p.calls.append(Call("open",
                                 [(PATH, self.new_path()), (IMM, O_RDWR | O_CREAT)],
                                 bind=("fd", [h])))
        self.live_fd.append(h)

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
        for _ in range(steps):
            choices = ["open", "pipe", "mmap", "getpid"]
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
