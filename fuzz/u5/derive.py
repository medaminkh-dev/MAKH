#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U5.3 — independent expected-result deriver.
#
# The C generator (user/ufuzz_u3_gen.h) attaches an expected result to every
# call it emits, and the guest executor checks the kernel against it. If the
# generator's model of a syscall is wrong, the oracle is wrong in the same way
# and the bug is invisible. This tool re-derives every expected result from a
# SEPARATE model and compares the two.
#
# Independence rules (keep them when editing this file):
#   - Predictions come from the program's calls and operands, the ABI inventory
#     (fuzz/abi/makh_syscalls.json: arg ctypes, documented errnos) and the
#     documented descriptor contract (docs/UFUZZ_U5_ORACLES.md §4.5).
#   - Numeric constants (O_*, F_*, SEEK_*, ARCH_*, AF_*/SOCK_*, errno, the
#     pipe capacity, the fd-table size) are read from the KERNEL headers and
#     sources, never from the generator.
#   - The generator's annotation (`=kind[:value]` in the IR) is read ONLY by
#     compare(); the Model never sees it.
#
# Verdict per call:
#   AGREE          the C expectation is exactly what the model predicts
#   WEAKER         the C expectation is satisfied by the prediction but admits
#                  more outcomes (e.g. nonneg where the model knows the slot):
#                  counted per syscall and reported — a measure of oracle
#                  strength, not a failure
#   UNPREDICTABLE  the model cannot predict the call (or the C expectation is
#                  stronger than anything the model can confirm): counted;
#                  FAILS in modes 5 and 6, whose programs are built to be fully
#                  predictable
#   CONTRADICTION  the C expectation rejects the predicted outcome: FAILS
# Every predicted -errno must also be in the inventory's documented set for
# that syscall (the model and the inventory must not drift apart).
#
#   derive.py [N]                 sweep every generator mode (N seeds for the
#                                 random modes, default 1024)
#   derive.py --mutation-selftest prove the check depends on BOTH sides: a
#                                 mutated C generator and a mutated deriver
#                                 must each produce contradictions

import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "fuzz", "u3"))
import u3  # noqa: E402

U4GEN = os.path.join(ROOT, "build", "u4gen")


# ---- kernel constants (the contract's numbers, from the kernel itself) -------
def _defines(path):
    out = {}
    with open(os.path.join(ROOT, path)) as f:
        for line in f:
            m = re.match(r"\s*#define\s+([A-Z_][A-Z0-9_]*)\s+\(?(-?(?:0x[0-9a-fA-F]+|\d+))\)?", line)
            if m:
                lit = m.group(2)
                neg_ = lit.startswith("-")
                body = lit.lstrip("-")
                if body.startswith(("0x", "0X")):
                    val = int(body, 16)
                elif len(body) > 1 and body.startswith("0"):
                    val = int(body, 8)               # C octal literal
                else:
                    val = int(body)
                out[m.group(1)] = -val if neg_ else val
    return out


K = {}
for _p in ("kernel/include/fs/vfs.h", "kernel/include/syscall.h",
           "kernel/include/net/socket.h", "kernel/fs/vfs.c"):
    K.update(_defines(_p))
ERRNO = _defines("kernel/include/errno.h")
ERRNAME = {v: k for k, v in ERRNO.items() if k.startswith("E")}

NFD = K["VFS_MAX_FDS"]                 # fd table size
PIPE_CAP = K["PIPE_CAP"]
ACCMODE = K["O_ACCMODE"] if "O_ACCMODE" in K else (K["O_WRONLY"] | K["O_RDWR"])
USER_TOP = 0x0000800000000000          # canonical lower half (x86-64 ABI)

# Descriptor-contract rules, kept as module attributes so the mutation
# selftest can perturb ONE rule and prove the comparison notices.
RULES = {
    # F_SETFL may change only the file status flags O_APPEND and O_NONBLOCK.
    "setfl_mask": K["O_APPEND"] | K["O_NONBLOCK"],
    # fds 0-2 are always open, so the lowest fd F_DUPFD can return is 3.
    "dupfd_min": 3,
    # a pipe read returns what is buffered, up to the request.
    "pipe_read": lambda want, buffered: min(want, buffered),
}


class Unknown(Exception):
    """The model cannot predict this call."""


def neg(name):
    return ("exact", -ERRNO[name])


def to_int(v, ctype):
    """Narrow a raw 64-bit operand to the ABI argument type."""
    v &= (1 << 64) - 1
    if ctype in ("int", "pid_t"):
        v &= 0xffffffff
        return v - (1 << 32) if v & 0x80000000 else v
    if ctype in ("long", "off_t", "ssize_t"):
        return v - (1 << 64) if v & (1 << 63) else v
    return v                                     # unsigned


# ---- the model ---------------------------------------------------------------
class Desc:
    __slots__ = ("kind", "flags", "path", "off", "pipe", "refs", "con_src")

    def __init__(self, kind, flags, path=None, pipe=None, con_src=None):
        self.kind, self.flags, self.path = kind, flags, path
        self.off, self.pipe, self.refs, self.con_src = 0, pipe, 0, con_src


class Pipe:
    __slots__ = ("buffered", "readers", "writers")

    def __init__(self):
        self.buffered, self.readers, self.writers = 0, 1, 1


class Model:
    """fd table in baseline ORDINALS: ordinal k is the k-th fd >= 3 that was
    free when the worker started. Absolute fds 0-2 are the console."""

    def __init__(self, prog, abi):
        self.p, self.abi = prog, abi
        need = prog.need
        self.cap = need[0] if (need and need[1]) else None   # exact slot count
        self.min_free = need[0] if need else 0               # guaranteed slots
        self.ords = {}                 # ordinal -> Desc
        self.hval = {}                 # handle -> ("o", k) | ("a", n) | ("bad",)
        self.addrs = set()             # live mapping handles
        self.files = {"/bin/hello": {"exists": True, "size": None},
                      "/mnt/ufz": {"exists": None, "size": None}}
        self.paths = dict(prog.paths)
        self.assumed = False           # this call relied on an undeclared free slot

    # -- fd helpers
    def lowest(self, k):
        """Lowest free ordinal >= k; None if the table is full; Unknown if the
        answer depends on slots the program did not declare."""
        o = max(k, 0)
        while True:
            if o in self.ords:
                o += 1
                continue
            if self.cap is not None:
                return o if o < self.cap else None
            if o < self.min_free:
                return o
            if self.p.need is None:
                # The program declares nothing: ASSUME the allocation succeeds
                # (the slot number is then not claimed — see derive_program)
                # and count it, so the report shows what rests on the assumption.
                self.assumed = True
                return o
            raise Unknown("allocation beyond the declared free slots")

    def ident(self, op, ctype):
        """Resolve an fd operand to ("o", k) / ("a", n) / ("bad",) / ("abs", n)."""
        kind = op[0]
        if kind == u3.RES:
            v = self.hval.get(op[1], ("bad",))
            if v[0] == "unknown":
                raise Unknown("handle bound to a value the model does not know")
            return v
        if kind == u3.SLOT:
            enc = op[1]
            k = enc & u3.SLOT_ORD_MASK
            hi = enc & ~u3.SLOT_ORD_MASK
            if ctype in ("int", "pid_t"):
                if hi & 0xffffffff:
                    raise Unknown("slot with bits inside the 32-bit fd value")
                return ("o", k)
            if hi:
                return ("bad",)
            return ("o", k)
        if kind == u3.IMM:
            v = to_int(op[1], ctype)
            if v < 0 or v >= NFD:
                return ("bad",)
            if v <= 2:
                return ("a", v)
            return ("abs", v)
        raise Unknown("fd operand of kind %r" % kind)

    def desc_of(self, ident):
        """The open description an identity names, or None if it is closed."""
        if ident[0] == "o":
            return self.ords.get(ident[1])
        if ident[0] == "a":
            return "console"
        if ident[0] == "abs":
            raise Unknown("absolute fd %d relative to an unknown baseline" % ident[1])
        return None

    def install(self, o, d):
        d.refs += 1
        self.ords[o] = d

    def release(self, o):
        d = self.ords.pop(o)
        d.refs -= 1
        if d.refs == 0 and d.kind == "pipe_r":
            d.pipe.readers -= 1
        if d.refs == 0 and d.kind == "pipe_w":
            d.pipe.writers -= 1

    def bind(self, c, idents):
        self._bound = True
        if c.bind is None:
            return
        for h, i in zip(c.bind[1], idents):
            self.hval[h] = i

    def fail_bind(self, c):
        self._bound = True
        if c.bind is not None:
            for h in c.bind[1]:
                self.hval[h] = ("bad",)

    def flags_of(self, ident, d):
        if d == "console":
            return ("basefl", ident[1])
        if d.kind == "console":
            return ("basefl", d.con_src)
        return ("exact", d.flags)

    def copy_for_dup(self, ident, d):
        if d == "console":
            return Desc("console", K["O_RDWR"], con_src=ident[1])
        return d

    # -- the syscalls
    def predict(self, c):
        sc = self.abi.by_name[c.sys]
        ctypes = [a.get("ctype") for a in sc.get("args", [])]
        fn = getattr(self, "sys_" + c.sys, None)
        if fn is None:
            raise Unknown("no model for %s" % c.sys)
        self._bound = False
        pred = fn(c, ctypes)
        if c.bind is not None and c.bind[0] == "fd" and len(c.bind[1]) == 1 and not self._bound:
            self._bind_result(c, pred)
        return pred

    def _bind_result(self, c, pred):
        """The executor's rule for an fd bind: the handle takes the call's return
        value when it is non-negative, else it is invalid — whatever the call is.
        A handler that creates an fd binds it itself; this covers every other
        call that carries a bind (e.g. a self-dup2, or a mutated fcntl whose
        command no longer creates an fd)."""
        h = c.bind[1][0]
        if pred[0] == "slot":
            self.hval[h] = ("o", pred[1])
        elif pred[0] == "exact" and pred[1] < 0:
            self.hval[h] = ("bad",)
        elif pred[0] == "exact":
            v = pred[1]
            self.hval[h] = ("a", v) if v <= 2 else ("abs", v)
        else:
            self.hval[h] = ("unknown",)

    def _imm(self, op, ctype):
        if op[0] != u3.IMM:
            raise Unknown("expected an immediate")
        return to_int(op[1], ctype)

    def _open(self, c, path_op, flags):
        path = self.paths.get(path_op[1]) if path_op[0] == u3.PATH else None
        if path is None:
            raise Unknown("open of a non-path operand")
        st = self.files.setdefault(path, {"exists": None, "size": None})
        if not (flags & K["O_CREAT"]):
            if st["exists"] is None:
                raise Unknown("existence of %s" % path)
            if not st["exists"]:
                self.fail_bind(c)
                return neg("ENOENT")
        o = self.lowest(0)
        if o is None:
            self.fail_bind(c)
            return neg("EMFILE")
        if flags & K["O_CREAT"] and not st["exists"]:
            st["exists"], st["size"] = True, 0
        if flags & K["O_TRUNC"] and (flags & ACCMODE) != K["O_RDONLY"]:
            st["size"] = 0
        d = Desc("file", flags & (ACCMODE | K["O_APPEND"] | K["O_NONBLOCK"]), path=path)
        self.install(o, d)
        self.bind(c, [("o", o)])
        return ("slot", o)

    def sys_open(self, c, t):
        return self._open(c, c.args[0], self._imm(c.args[1], t[1]))

    def sys_openat(self, c, t):
        # An absolute path ignores dirfd.
        path = self.paths.get(c.args[1][1]) if c.args[1][0] == u3.PATH else ""
        if not path.startswith("/"):
            raise Unknown("relative openat")
        return self._open(c, c.args[1], self._imm(c.args[2], t[2]))

    def sys_close(self, c, t):
        i = self.ident(c.args[0], t[0])
        if i[0] == "bad":
            return neg("EBADF")
        if i[0] == "a":
            return ("exact", 0)                  # implicit console: nothing to free
        if i[0] == "abs":
            raise Unknown("close of an absolute fd")
        if i[1] not in self.ords:
            return neg("EBADF")
        self.release(i[1])
        return ("exact", 0)

    def sys_read(self, c, t):
        i = self.ident(c.args[0], t[0])
        n = self._imm(c.args[2], t[2])
        d = self.desc_of(i)
        if d is None:
            return neg("EBADF")
        if d == "console" or d.kind in ("console", "sock"):
            raise Unknown("read of a %s" % ("console" if d == "console" else d.kind))
        if d.kind == "pipe_w" or (d.kind == "file" and (d.flags & ACCMODE) == K["O_WRONLY"]):
            return neg("EBADF")
        if n == 0:
            return ("exact", 0)
        if d.kind == "pipe_r":
            p = d.pipe
            if p.buffered == 0 and p.writers > 0:
                raise Unknown("read would block")
            got = RULES["pipe_read"](n, p.buffered)
            p.buffered -= got
            return ("exact", got)
        size = self.files[d.path]["size"]
        if size is None:
            return ("nonneg",)
        got = max(0, min(n, size - d.off))
        d.off += got
        return ("exact", got)

    def sys_write(self, c, t):
        i = self.ident(c.args[0], t[0])
        n = self._imm(c.args[2], t[2])
        d = self.desc_of(i)
        if d is None:
            return neg("EBADF")
        if d == "console" or d.kind in ("console", "sock"):
            raise Unknown("write to a console/socket")
        if d.kind == "pipe_r" or (d.kind == "file" and (d.flags & ACCMODE) == K["O_RDONLY"]):
            return neg("EBADF")
        if n == 0:
            return ("exact", 0)
        if d.kind == "pipe_w":
            p = d.pipe
            if p.readers == 0:
                raise Unknown("write to a pipe with no reader (SIGPIPE)")
            if p.buffered + n > PIPE_CAP:
                raise Unknown("write would block")
            p.buffered += n
            return ("exact", n)
        st = self.files[d.path]
        if d.flags & K["O_APPEND"]:
            if st["size"] is None:
                raise Unknown("append to a file of unknown size")
            d.off = st["size"]
        d.off += n
        if st["size"] is not None:
            st["size"] = max(st["size"], d.off)
        return ("exact", n)

    def sys_lseek(self, c, t):
        i = self.ident(c.args[0], t[0])
        off = self._imm(c.args[1], t[1])
        whence = self._imm(c.args[2], t[2])
        d = self.desc_of(i)
        if d is None:
            return neg("EBADF")
        if d == "console" or d.kind != "file":
            raise Unknown("lseek on a non-file")
        if whence == K["SEEK_SET"]:
            base = 0
        elif whence == K["SEEK_CUR"]:
            base = d.off
        else:
            base = self.files[d.path]["size"]
            if base is None:
                raise Unknown("SEEK_END of unknown size")
        if base + off < 0:
            return neg("EINVAL")
        d.off = base + off
        return ("exact", d.off)

    def sys_unlink(self, c, t):
        path = self.paths.get(c.args[0][1]) if c.args[0][0] == u3.PATH else None
        st = self.files.get(path)
        if st is None or st["exists"] is None:
            raise Unknown("existence of %s" % path)
        if not st["exists"]:
            return neg("ENOENT")
        st["exists"], st["size"] = False, None
        return ("exact", 0)

    def sys_getpid(self, c, t):
        return ("nonneg",)

    def sys_mmap(self, c, t):
        length = self._imm(c.args[1], t[1])
        flags = self._imm(c.args[3], t[3])
        if length > 0 and flags & K["MAP_ANONYMOUS"]:
            self.bind(c, [("addr",)])
            if c.bind:
                self.addrs.add(c.bind[1][0])
            return ("nonneg",)
        raise Unknown("non-anonymous mmap")

    def sys_mprotect(self, c, t):
        if c.args[0][0] == u3.RES and c.args[0][1] in self.addrs:
            return ("exact", 0)
        raise Unknown("mprotect of an unknown range")

    def sys_munmap(self, c, t):
        if c.args[0][0] == u3.RES and c.args[0][1] in self.addrs:
            self.addrs.discard(c.args[0][1])
            return ("exact", 0)
        raise Unknown("munmap of an unknown range")

    def sys_arch_prctl(self, c, t):
        code = self._imm(c.args[0], t[0])
        if code == K["ARCH_SET_FS"]:
            if c.args[1][0] != u3.IMM:
                raise Unknown("SET_FS base not an immediate")
            return ("exact", 0) if 0 <= c.args[1][1] < USER_TOP else neg("EPERM")
        if code == K["ARCH_GET_FS"]:
            return ("exact", 0) if c.args[1][0] == u3.BUF else neg("EFAULT")
        return neg("EINVAL")

    def _pipe(self, c, t):
        op = c.args[0]
        if op[0] == u3.BAD or op[0] == u3.NIL:
            return neg("EFAULT")
        if op[0] != u3.BUF:
            raise Unknown("pipe buffer operand")
        o1 = self.lowest(0)
        if o1 is None:
            self.fail_bind(c)
            return neg("EMFILE")
        r = Desc("pipe_r", K["O_RDONLY"], pipe=Pipe())
        self.install(o1, r)                      # read end first
        o2 = self.lowest(o1 + 1)
        if o2 is None:                           # write end fails: roll back
            self.ords.pop(o1)
            self.fail_bind(c)
            return neg("EMFILE")
        self.install(o2, Desc("pipe_w", K["O_WRONLY"], pipe=r.pipe))
        self.bind(c, [("o", o1), ("o", o2)])
        return ("exact", 0)

    sys_pipe = _pipe
    sys_pipe2 = _pipe                            # flags are not honoured (partial)

    def sys_dup2(self, c, t):
        old = self.ident(c.args[0], t[0])
        d = self.desc_of(old)
        if d is None:
            self.fail_bind(c)
            return neg("EBADF")
        new = self.ident(c.args[1], t[1])
        if new[0] == "bad":
            self.fail_bind(c)
            return neg("EBADF")
        if new == old:
            return ("slot", new[1]) if new[0] == "o" else ("exact", new[1])
        if new[0] != "o":
            raise Unknown("dup2 onto fd %r" % (new,))
        src = self.copy_for_dup(old, d)
        if new[1] in self.ords:
            src.refs += 1                        # hold src across the release
            self.release(new[1])
            src.refs -= 1
        self.install(new[1], src)
        self.bind(c, [new])
        return ("slot", new[1])

    def sys_fcntl(self, c, t):
        i = self.ident(c.args[0], t[0])
        cmd = self._imm(c.args[1], t[1])
        d = self.desc_of(i)
        if d is None:
            return neg("EBADF")
        if cmd == K["F_GETFL"]:
            return self.flags_of(i, d)
        if cmd == K["F_SETFL"]:
            arg = self._imm(c.args[2], t[2])
            if d == "console" or d.kind == "console":
                raise Unknown("F_SETFL on the shared console")
            m = RULES["setfl_mask"]
            d.flags = (d.flags & ~m) | (arg & m)
            return ("exact", 0)
        if cmd in (K["F_GETFD"], K["F_SETFD"]):
            return ("exact", 0)                  # FD_CLOEXEC not tracked (partial)
        if cmd == K["F_DUPFD"]:
            a = c.args[2]
            if a[0] == u3.SLOT:
                if a[1] & ~u3.SLOT_ORD_MASK:
                    raise Unknown("F_DUPFD floor with high bits")
                floor_ord = a[1]
            else:
                v = self._imm(a, "long")         # the floor is judged as a signed long
                if v < 0:
                    return neg("EINVAL")
                if v >= NFD:
                    return neg("EMFILE")
                if v > 3:
                    raise Unknown("absolute F_DUPFD floor %d" % v)
                if v >= RULES["dupfd_min"] and v < 3:
                    # Only reachable under a mutated rule that treats fds 0-2
                    # as allocatable: it would hand back fd v itself.
                    return ("exact", v)
                floor_ord = 0                    # lowest free fd >= 3 = ordinal 0
            o = self.lowest(floor_ord)
            if o is None:
                self.fail_bind(c)
                return neg("EMFILE")
            self.install(o, self.copy_for_dup(i, d))
            self.bind(c, [("o", o)])
            return ("slot", o)
        return neg("EINVAL")

    def sys_socket(self, c, t):
        dom = self._imm(c.args[0], t[0])
        typ = self._imm(c.args[1], t[1])
        if dom != K["AF_INET"]:
            self.fail_bind(c)
            return neg("EAFNOSUPPORT")
        if typ != K["SOCK_DGRAM"]:
            raise Unknown("socket type %d" % typ)
        o = self.lowest(0)
        if o is None:
            self.fail_bind(c)
            return neg("EMFILE")
        self.install(o, Desc("sock", K["O_RDWR"]))
        self.bind(c, [("o", o)])
        return ("slot", o)


# ---- comparison -------------------------------------------------------------
def compare(pred, cexp):
    """Return AGREE / WEAKER / UNPREDICTABLE / CONTRADICTION."""
    ck = cexp[0] if cexp else "any"
    pk = pred[0]
    if ck == "any":
        return "WEAKER"
    if pk == "nonneg":
        if ck == "nonneg":
            return "AGREE"
        if ck == "neg" or (ck == "exact" and cexp[1] < 0):
            return "CONTRADICTION"
        return "UNPREDICTABLE"                   # C claims more than can be confirmed
    if pk == "exact":
        v = pred[1]
        if ck == "exact":
            return "AGREE" if cexp[1] == v else "CONTRADICTION"
        if ck == "nonneg":
            return "WEAKER" if v >= 0 else "CONTRADICTION"
        if ck == "neg":
            return "WEAKER" if v < 0 else "CONTRADICTION"
        return "CONTRADICTION"                   # slot/basefl vs a literal result
    if pk == "slot":
        if ck == "slot":
            return "AGREE" if cexp[1] == pred[1] else "CONTRADICTION"
        if ck == "nonneg":
            return "WEAKER"
        return "CONTRADICTION"
    if pk == "basefl":
        if ck == "basefl":
            return "AGREE" if cexp[1] == pred[1] else "CONTRADICTION"
        if ck == "nonneg":
            return "WEAKER"
        return "CONTRADICTION"
    return "CONTRADICTION"


class Report:
    def __init__(self):
        self.verdicts = Counter()
        self.weaker = Counter()                  # syscall -> count
        self.unpred = Counter()                  # (syscall, reason) -> count
        self.contra = []                         # (label, seed, idx, sys, pred, cexp)
        self.errno_drift = []
        self.assumed = Counter()                 # syscall -> allocations assumed to succeed
        self.programs = 0


def derive_program(prog, abi, label, seed, rep):
    m = Model(prog, abi)
    rep.programs += 1
    for idx, c in enumerate(prog.calls):
        m.assumed = False
        try:
            pred = m.predict(c)
            if m.assumed:
                # Success was assumed, not derived: claim only "a new fd".
                rep.assumed[c.sys] += 1
                if pred[0] == "slot":
                    pred = ("nonneg",)
        except Unknown as e:
            rep.verdicts["UNPREDICTABLE"] += 1
            rep.unpred[(c.sys, str(e))] += 1
            # Later calls may depend on what this one did: stop here.
            return
        if pred[0] == "exact" and pred[1] < 0:
            name = ERRNAME.get(-pred[1], str(-pred[1]))
            if name not in abi.by_name[c.sys].get("errno", []):
                rep.errno_drift.append((label, seed, idx, c.sys, name))
        v = compare(pred, c.expect)
        rep.verdicts[v] += 1
        if v == "WEAKER":
            rep.weaker[c.sys] += 1
        elif v == "UNPREDICTABLE":
            rep.unpred[(c.sys, "C expectation stronger than the model can confirm")] += 1
        elif v == "CONTRADICTION":
            rep.contra.append((label, seed, idx, c.sys, pred, c.expect))


def run_u4gen(binary, mode, n):
    out = subprocess.run([binary, "0", str(n), str(mode)], capture_output=True,
                         text=True, check=True).stdout
    progs, seed, cur = [], None, []
    for line in out.splitlines():
        if line.startswith("### seed"):
            if seed is not None:
                progs.append((seed, "\n".join(cur)))
            seed, cur = int(line.split()[2]), []
        elif seed is not None:
            cur.append(line)
    if seed is not None:
        progs.append((seed, "\n".join(cur)))
    if len(progs) != n:
        raise RuntimeError("mode %d: expected %d programs, parsed %d" % (mode, n, len(progs)))
    return progs


# (mode, label, random?, must_be_fully_predictable, is_negative_control)
MODES = [(0, "skeleton", True, False, False), (1, "ext2", True, False, False),
         (2, "ext2-direct", True, False, False), (3, "emfile", False, False, False),
         (4, "emfile-control", False, False, True), (5, "fdops", True, True, False),
         (6, "pipe-rollback", False, True, False)]


def sweep(binary, n, abi, modes=MODES):
    reps = {}
    for mode, label, rnd, _, _ in modes:
        rep = Report()
        for seed, ir in run_u4gen(binary, mode, n if rnd else 1):
            prog = u3.deserialize(ir)
            # The annotated IR must round-trip, or the comparison could be
            # reading a different expectation than the generator wrote.
            if u3.serialize(prog) != ir.strip("\n") + "\n":
                raise RuntimeError("mode %d seed %d: annotated IR does not round-trip"
                                   % (mode, seed))
            derive_program(prog, abi, label, seed, rep)
        reps[mode] = rep
    return reps


def fail(msg):
    print("U5 DERIVE-CHECK: FAIL — " + msg, file=sys.stderr)
    sys.exit(1)


# ---- negative cases: a wrong annotation must be a contradiction -------------
NEGATIVES = [
    ("slot off by one",
     "abi 1\nprng lcg 0\nhandles fd\npath 0 /bin/hello\nneed 4 min\n"
     "call open path:0 imm:0 -> fd:0 =slot:1\ncall close res:0 =exact:0\n"),
    ("-EBADF where 0 is due",
     "abi 1\nprng lcg 0\nhandles fd\npath 0 /bin/hello\nneed 4 min\n"
     "call open path:0 imm:0 -> fd:0 =slot:0\ncall close res:0 =exact:-%d\n" % ERRNO["EBADF"]),
    ("F_SETFL keeps an access bit",
     "abi 1\nprng lcg 0\nhandles fd\npath 0 /bin/hello\nneed 4 min\n"
     "call open path:0 imm:0 -> fd:0 =slot:0\n"
     "call fcntl res:0 imm:%d imm:%d =exact:0\n"
     "call fcntl res:0 imm:%d imm:0 =exact:%d\n"
     "call close res:0 =exact:0\n" % (K["F_SETFL"], K["O_APPEND"] | K["O_RDWR"],
                                      K["F_GETFL"], K["O_APPEND"] | K["O_RDWR"])),
]


# ---- positive cases: correct annotations the model must AGREE with -----------
# The executor binds an fd handle to whatever non-negative value the call
# returns, whatever the call is. U8's mutations reached two shapes the C
# generator never emits, and the real kernel's results there are the truth:
POSITIVES = [
    ("self-dup2 binds its handle",
     "abi 1\nprng lcg 0\nhandles fd fd\npath 0 /bin/hello\nneed 4 min\n"
     "call open path:0 imm:0 -> fd:0 =slot:0\n"
     "call dup2 res:0 slot:0 -> fd:1 =slot:0\n"
     "call close res:1 =exact:0\n"
     "call close res:0 =exact:-%d\n" % ERRNO["EBADF"]),
    ("a non-creating fcntl with a bind takes its return value",
     "abi 1\nprng lcg 0\nhandles fd fd\npath 0 /bin/hello\nneed 4 min\n"
     "call open path:0 imm:0 -> fd:0 =slot:0\n"
     "call fcntl res:0 imm:%d imm:0 -> fd:1 =exact:0\n"
     "call close res:1 =exact:0\n"
     "call close res:0 =exact:0\n" % K["F_GETFD"]),
]


def check_positives(abi):
    for name, ir in POSITIVES:
        rep = Report()
        derive_program(u3.deserialize(ir), abi, "pos", 0, rep)
        if rep.contra or rep.verdicts["UNPREDICTABLE"]:
            fail("positive case %r: the model disagrees with the executor's binding rule "
                 "(%d contradiction(s), %d unpredictable)"
                 % (name, len(rep.contra), rep.verdicts["UNPREDICTABLE"]))
    return len(POSITIVES)


def check_negatives(abi):
    for name, ir in NEGATIVES:
        rep = Report()
        derive_program(u3.deserialize(ir), abi, "neg", 0, rep)
        if not rep.contra:
            fail("negative case %r was not detected" % name)
    return len(NEGATIVES)


def main(n):
    abi = u3.Abi()
    if not os.path.exists(U4GEN):
        fail("%s is not built (run `make u5-derive-check`)" % os.path.relpath(U4GEN, ROOT))
    reps = sweep(U4GEN, n, abi)
    problems = []
    for mode, label, _, strict, control in MODES:
        r = reps[mode]
        if control:
            if not r.contra:
                problems.append("mode %d (%s): the negative control produced no "
                                "contradiction" % (mode, label))
            continue
        if r.contra:
            lab, seed, idx, sysn, pred, cexp = r.contra[0]
            problems.append("mode %d (%s): %d contradiction(s); first: seed %d call %d "
                            "%s predicted %r, C expects %r"
                            % (mode, label, len(r.contra), seed, idx, sysn, pred, cexp))
        if strict and r.verdicts["UNPREDICTABLE"]:
            (sysn, why), cnt = r.unpred.most_common(1)[0]
            problems.append("mode %d (%s): %d unpredictable call(s); e.g. %s: %s"
                            % (mode, label, r.verdicts["UNPREDICTABLE"], sysn, why))
        if r.errno_drift:
            problems.append("mode %d (%s): predicted errno not documented: %r"
                            % (mode, label, r.errno_drift[0]))
    nneg = check_negatives(abi)
    npos = check_positives(abi)

    for mode, label, _, _, control in MODES:
        r = reps[mode]
        v = r.verdicts
        line = ("  mode %d %-14s programs=%-5d agree=%-6d weaker=%-5d unpredictable=%-4d "
                "contradiction=%d%s" % (mode, label, r.programs, v["AGREE"], v["WEAKER"],
                                        v["UNPREDICTABLE"], v["CONTRADICTION"],
                                        "  (negative control)" if control else ""))
        print(line)
        if r.weaker:
            print("      weaker by syscall: " + ", ".join(
                "%s=%d" % kv for kv in sorted(r.weaker.items())))
        if r.assumed:
            print("      assumed free slot (no `need`): " + ", ".join(
                "%s=%d" % kv for kv in sorted(r.assumed.items())))
        if r.unpred:
            print("      unpredictable: " + "; ".join(
                "%s (%s)=%d" % (s, w, k) for (s, w), k in sorted(r.unpred.items())))
    if problems:
        fail("; ".join(problems))
    total = sum(r.programs for r in reps.values())
    print("U5 DERIVE-CHECK: PASS — %d programs (%d modes) re-derived independently; "
          "no contradiction outside the negative control; %d negative cases detected, "
          "%d positive cases agree." % (total, len(MODES), nneg, npos))


# ---- mutation selftest: the check must depend on BOTH sides -----------------
C_MUTATIONS = [
    ("F_SETFL keeps access-mode bits",
     "int keep = U3G_O_APPEND | U3G_O_NONBLOCK;",
     "int keep = U3G_O_APPEND | U3G_O_NONBLOCK | U3G_O_RDWR | U3G_O_WRONLY;"),
    ("pipe read expects the full request",
     "int got = len < p->buffered ? len : p->buffered;",
     "int got = len;"),
    ("dup2 onto a live fd expects the source slot",
     "u3g_set_fd(c, 0, so); u3g_set_fd(c, 1, dob);\n    c->expect_mode = U3_SLOT; c->expect_val = dob;",
     "u3g_set_fd(c, 0, so); u3g_set_fd(c, 1, dob);\n    c->expect_mode = U3_SLOT; c->expect_val = so;"),
]

PY_MUTATIONS = [
    ("F_SETFL mask includes access-mode bits", "setfl_mask",
     K["O_APPEND"] | K["O_NONBLOCK"] | ACCMODE),
    ("pipe read returns the full request", "pipe_read", lambda want, buffered: want),
    ("F_DUPFD floor 0..2 may return 0..2", "dupfd_min", 0),
]


def mutation_selftest(n=256):
    abi = u3.Abi()
    fd_mode = [m for m in MODES if m[0] == 5]
    results = []
    tmp = tempfile.mkdtemp(prefix="u5mut")
    try:
        for f in ("user/ufuzz_u3_gen.h", "user/ufuzz_u3_programs.h"):
            shutil.copy(os.path.join(ROOT, f), tmp)
        gen_src = open(os.path.join(tmp, "ufuzz_u3_gen.h")).read()
        for name, old, new in C_MUTATIONS:
            if gen_src.count(old) != 1:
                fail("C mutation %r: anchor not found exactly once" % name)
            with open(os.path.join(tmp, "ufuzz_u3_gen.h"), "w") as fh:
                fh.write(gen_src.replace(old, new))
            binary = os.path.join(tmp, "u4gen_mut")
            subprocess.run(["cc", "-I", tmp, "-w", "-O2", "-o", binary,
                            os.path.join(ROOT, "fuzz", "u4", "u4gen.c")], check=True)
            k = len(sweep(binary, n, abi, fd_mode)[5].contra)
            results.append(("C", name, k))
        with open(os.path.join(tmp, "ufuzz_u3_gen.h"), "w") as fh:
            fh.write(gen_src)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if not os.path.exists(U4GEN):
        fail("%s is not built" % os.path.relpath(U4GEN, ROOT))
    for name, key, val in PY_MUTATIONS:
        saved = RULES[key]
        RULES[key] = val
        try:
            k = len(sweep(U4GEN, n, abi, fd_mode)[5].contra)
        finally:
            RULES[key] = saved
        results.append(("deriver", name, k))
    base = len(sweep(U4GEN, n, abi, fd_mode)[5].contra)

    for side, name, k in results:
        print("  %-7s mutation %-46s -> %d contradiction(s) in %d mode-5 programs"
              % (side, repr(name), k, n))
    print("  unmutated baseline -> %d contradiction(s)" % base)
    missed = [r for r in results if r[2] == 0]
    if base or missed:
        fail("mutation selftest: baseline=%d, undetected=%r" % (base, [m[1] for m in missed]))
    print("U5 DERIVE MUTATION SELFTEST: PASS — %d generator and %d deriver mutations "
          "each produce contradictions; the unmutated pair agrees."
          % (len(C_MUTATIONS), len(PY_MUTATIONS)))


if __name__ == "__main__":
    if "--mutation-selftest" in sys.argv[1:]:
        mutation_selftest()
    else:
        args = [a for a in sys.argv[1:] if not a.startswith("-")]
        main(int(args[0]) if args else 1024)
