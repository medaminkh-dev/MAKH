# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U3 — typed syscall program model (host-side).
#
# A program is an ordered list of Calls over a small, concrete operand model.
# This module is the single host-side implementation of the IR, its text
# serializer/deserializer, an ABI-driven generator, and a static validator.
# It is driven by fuzz/abi/makh_syscalls.json (U0), so it cannot describe an
# argument shape the ABI does not. No kernel build is required — it runs in CI.
#
# MVP operand kinds (per the approved U3 design, §6): imm, res, null, buf, path.
# Syscalls whose ABI args are `variant`/struct/array are out of the MVP
# generator's scope (deferred to U3.1); the validator still checks any program.

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ABI_PATH = os.path.join(ROOT, "fuzz", "abi", "makh_syscalls.json")

ABI_VERSION = 1

# The guest executor (user/ufuzz.c) holds at most this many segments per iovec
# operand. It is the SINGLE source of truth for that limit: emit.py imports it
# (and bakes `#define U3_MAX_SEG` from it) and the validator rejects any iov
# with more, so validator, emitter and guest cannot disagree. It is well under
# the kernel's own iovcnt cap (1024), which the validator also enforces.
MAX_SEG = 4

# ---- operands -------------------------------------------------------------
# An operand is a small tuple tagged by kind:
#   ("imm", int) | ("res", handle_id) | ("null",) | ("buf", buf_id)
#   | ("path", path_id) | ("iov", iov_id)
# An `iov` operand (U3.1b) refers to a program-level iovec spec: an ordered list
# of (buf_id, len) segments the guest materializes into a struct iovec[] at
# runtime (each iov_base is the runtime address of that arena buffer — a thing
# the host can't know, hence a dedicated operand rather than a flat buffer).
IMM, RES, NIL, BUF, PATH, IOV = "imm", "res", "null", "buf", "path", "iov"
# U5.2 fd-surface operands:
#   ("slot", enc) — a descriptor NUMBER chosen relative to the worker's baseline:
#       the executor resolves the low 16 bits as an ordinal into the fds that were
#       FREE (>= 3) when the program started, and ORs back the remaining bits (so
#       enc = k | 1<<32 tests int truncation). Used for fds the program does not
#       hold a handle on: a free slot, a stale (closed) slot, a dup2 target.
#   ("bad", addr) — a deliberately invalid pointer (NULL, kernel-half,
#       non-canonical, unmapped). Allowed only for a pointer arg; the call is
#       expected to be refused (-EFAULT), never to dereference it.
SLOT, BAD = "slot", "bad"
SLOT_ORD_MASK = 0xffff


class Call:
    __slots__ = ("sys", "args", "bind", "tag")

    def __init__(self, sys, args, bind=None, tag=None):
        self.sys = sys              # syscall name (matches the ABI inventory)
        self.args = args            # list of operand tuples
        self.bind = bind            # None, or (rtype, [handle_id,...]) created by this call
        self.tag = tag              # None, "after_release", or "before_create"


class Program:
    __slots__ = ("abi", "prng", "buffers", "paths", "calls", "handles", "iovs")

    def __init__(self, abi=ABI_VERSION, prng=("lcg", 0)):
        self.abi = abi
        self.prng = prng            # (algo, seed)
        self.buffers = []           # list of (id, size, bytes)
        self.paths = []             # list of (id, str)
        self.calls = []             # list of Call
        self.handles = []           # list of rtype, index == handle id
        self.iovs = []              # list of [ (buf_id, len), ... ]  (index == iov id)


# ---- ABI access -----------------------------------------------------------
class Abi:
    def __init__(self, path=ABI_PATH):
        with open(path) as f:
            self.data = json.load(f)
        self.by_name = {s["name"]: s for s in self.data["syscalls"]}
        self.struct_defs = self.data.get("struct_defs", {})

    def sc(self, name):
        return self.by_name[name]

    def arg_is_mvp(self, a):
        """True if this argument is representable with the five MVP operand
        kinds (so the MVP generator can emit it)."""
        k = a.get("kind")
        if k == "scalar":
            return True
        if k == "ptr":
            # flat buffer or cstr/path; not a nested struct/array (deferred)
            sz = a.get("size", "")
            return not (sz.startswith("arr:") or sz.startswith("struct:"))
        return False  # variant: deferred

    def sc_is_mvp(self, name):
        sc = self.by_name.get(name)
        if not sc or sc.get("impl") == "unsupported":
            return False
        return all(self.arg_is_mvp(a) for a in sc.get("args", []))


# ---- text serialization ---------------------------------------------------
def _op_str(op):
    k = op[0]
    if k == IMM:
        return "imm:%d" % op[1]
    if k == RES:
        return "res:%d" % op[1]
    if k == NIL:
        return "null"
    if k == BUF:
        return "buf:%d" % op[1]
    if k == PATH:
        return "path:%d" % op[1]
    if k == IOV:
        return "iov:%d" % op[1]
    if k == SLOT:
        return "slot:%d" % op[1]
    if k == BAD:
        return "bad:%d" % op[1]
    raise ValueError("bad operand %r" % (op,))


def _op_parse(tok):
    if tok == "null":
        return (NIL,)
    k, _, v = tok.partition(":")
    if k == IMM:
        return (IMM, int(v))
    if k in (RES, BUF, PATH, IOV, SLOT, BAD):
        return (k, int(v))
    raise ValueError("bad operand token %r" % tok)


def serialize(p):
    """Render a Program to the canonical text IR (deterministic)."""
    out = []
    out.append("abi %d" % p.abi)
    out.append("prng %s %d" % (p.prng[0], p.prng[1]))
    out.append("handles %s" % (" ".join(p.handles) if p.handles else "-"))
    for bid, size, data in p.buffers:
        out.append("buf %d %d %s" % (bid, size, data.hex()))
    for pid, s in p.paths:
        out.append("path %d %s" % (pid, s))
    for iid, segs in enumerate(p.iovs):
        out.append("iov %d %s" % (iid, " ".join("%d:%d" % (b, l) for b, l in segs)))
    for c in p.calls:
        line = "call %s" % c.sys
        for op in c.args:
            line += " " + _op_str(op)
        if c.bind is not None:
            line += " -> %s:%s" % (c.bind[0], ",".join(str(h) for h in c.bind[1]))
        if c.tag:
            line += " #%s" % c.tag
        out.append(line)
    return "\n".join(out) + "\n"


def deserialize(text):
    """Parse the canonical text IR back into a Program."""
    p = Program()
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        head = parts[0]
        if head == "abi":
            p.abi = int(parts[1])
        elif head == "prng":
            p.prng = (parts[1], int(parts[2]))
        elif head == "handles":
            p.handles = [] if parts[1:] == ["-"] else parts[1:]
        elif head == "buf":
            p.buffers.append((int(parts[1]), int(parts[2]),
                              bytes.fromhex(parts[3]) if len(parts) > 3 else b""))
        elif head == "path":
            p.paths.append((int(parts[1]), parts[2]))
        elif head == "iov":
            # "iov <id> b:l b:l ..." — id is positional (append in order)
            segs = [tuple(int(x) for x in seg.split(":")) for seg in parts[2:]]
            p.iovs.append(segs)
        elif head == "call":
            sys = parts[1]
            args, bind, tag = [], None, None
            i = 2
            while i < len(parts):
                t = parts[i]
                if t == "->":
                    rtype, _, hs = parts[i + 1].partition(":")
                    bind = (rtype, [int(x) for x in hs.split(",")] if hs else [])
                    i += 2
                    continue
                if t.startswith("#"):
                    tag = t[1:]
                    i += 1
                    continue
                args.append(_op_parse(t))
                i += 1
            p.calls.append(Call(sys, args, bind, tag))
        else:
            raise ValueError("bad IR line %r" % line)
    return p


# ---- validator ------------------------------------------------------------
LIVE, RELEASED = "LIVE", "RELEASED"


class ValErr(Exception):
    pass


def validate(p, abi):
    """Static validation. Raises ValErr on the first problem; returns a small
    stats dict on success. Checks operand kinds against the ABI, and the
    resource lifecycle (no use-before-create / use-after-release unless the
    call carries the matching mutation tag)."""
    hstate = {}            # handle id -> LIVE/RELEASED
    created_by = {}        # handle id -> call index that created it
    bufids = {b[0] for b in p.buffers}
    bufsize = {b[0]: b[1] for b in p.buffers}
    pathids = {pp[0] for pp in p.paths}
    dep_edges = 0

    for idx, c in enumerate(p.calls):
        sc = abi.by_name.get(c.sys)
        if sc is None:
            raise ValErr("call %d: unknown syscall %r" % (idx, c.sys))
        specs = sc.get("args", [])
        if len(c.args) != len(specs):
            raise ValErr("call %d (%s): %d operands, ABI wants %d"
                         % (idx, c.sys, len(c.args), len(specs)))

        for ai, (op, spec) in enumerate(zip(c.args, specs)):
            kind = op[0]
            akind = spec.get("kind")
            # A `variant` arg is validated against the CONCRETE shape its
            # selector selects (e.g. ioctl argp given request==TCGETS is an out
            # ptr to struct termios). Resolve it so `akind`/`msize_spec` reflect
            # that shape. An unknown/undecidable selector leaves the operand
            # unconstrained — such a call just returns -errno at runtime, which
            # is a legitimate RESULT, not a structural violation.
            msize_spec = spec
            if akind == "variant":
                vshape = _resolve_variant(spec, c, specs)
                if vshape is None:
                    continue            # unconstrained: accept any operand here
                akind = vshape.get("kind")
                msize_spec = vshape

            # Every table-index operand must carry a real, non-negative id: a
            # negative one would index a Python list from the end here (a silent
            # wrong-reference) and read out of bounds in the guest.
            if kind in (RES, BUF, PATH, IOV) and op[1] < 0:
                raise ValErr("call %d (%s) arg %d: negative %s id %d"
                             % (idx, c.sys, ai, kind, op[1]))

            if kind == RES:
                # A resource handle may only stand in for a scalar arg that NAMES
                # a resource, and its declared type must match — no fd where the
                # ABI wants an addr, etc. (operand-kind + resource-type check).
                if akind != "scalar" or not spec.get("res"):
                    raise ValErr("call %d (%s) arg %d: res for an arg that is not a scalar "
                                 "resource (kind=%s res=%r)" % (idx, c.sys, ai, akind, spec.get("res")))
                h = op[1]
                if h >= len(p.handles):
                    raise ValErr("call %d (%s) arg %d: res %d is not a declared handle"
                                 % (idx, c.sys, ai, h))
                if p.handles[h] != spec.get("res"):
                    raise ValErr("call %d (%s) arg %d: res %d is a %r handle but %s wants %r"
                                 % (idx, c.sys, ai, h, p.handles[h], spec.get("name"), spec.get("res")))
                if h not in created_by or created_by[h] >= idx:
                    if c.tag != "before_create":
                        raise ValErr("call %d (%s) arg %d: res %d used before creation"
                                     % (idx, c.sys, ai, h))
                elif hstate.get(h) == RELEASED and c.tag != "after_release":
                    raise ValErr("call %d (%s) arg %d: res %d used after release"
                                 % (idx, c.sys, ai, h))
                else:
                    dep_edges += 1
            elif kind == BUF:
                # A flat buffer fills a non-array pointer (an array arg takes an
                # iov; a scalar takes an imm/res).
                if akind != "ptr" or msize_spec.get("size", "").startswith("arr:"):
                    raise ValErr("call %d (%s) arg %d: buf for a non-(flat-ptr) ABI arg "
                                 "(kind=%s size=%s)" % (idx, c.sys, ai, akind, msize_spec.get("size", "")))
                if op[1] not in bufids:
                    raise ValErr("call %d (%s) arg %d: buf %d does not exist"
                                 % (idx, c.sys, ai, op[1]))
                need = _min_size(msize_spec, c, p, specs)
                if need is not None and bufsize[op[1]] < need:
                    raise ValErr("call %d (%s) arg %d: buf %d is %d bytes, need %d"
                                 % (idx, c.sys, ai, op[1], bufsize[op[1]], need))
            elif kind == PATH:
                if akind != "ptr":
                    raise ValErr("call %d (%s) arg %d: path for a non-ptr ABI arg (%s)"
                                 % (idx, c.sys, ai, akind))
                if op[1] not in pathids:
                    raise ValErr("call %d (%s) arg %d: path %d does not exist"
                                 % (idx, c.sys, ai, op[1]))
            elif kind == IOV:
                # An iovec array argument (readv/writev). The ABI arg must be a
                # ptr whose size rule is `arr:<countarg>:<elem>`; the referenced
                # iov spec must fit the guest executor (MAX_SEG) and the kernel
                # cap, its count must equal the sibling count operand (an IMM),
                # and every segment must name an existing buffer at least as
                # large as its (non-negative) declared len.
                size = spec.get("size", "")
                if akind != "ptr" or not size.startswith("arr:"):
                    raise ValErr("call %d (%s) arg %d: iov for a non-array ABI arg (%s %s)"
                                 % (idx, c.sys, ai, akind, size))
                if op[1] >= len(p.iovs):
                    raise ValErr("call %d (%s) arg %d: iov %d does not exist"
                                 % (idx, c.sys, ai, op[1]))
                _, countarg, _ = (size.split(":") + ["", ""])[:3]
                segs = p.iovs[op[1]]
                if len(segs) > MAX_SEG:
                    raise ValErr("call %d (%s) arg %d: iov %d has %d segments (executor cap %d)"
                                 % (idx, c.sys, ai, op[1], len(segs), MAX_SEG))
                if len(segs) > 1024:
                    raise ValErr("call %d (%s) arg %d: iov %d has %d segments (kernel cap 1024)"
                                 % (idx, c.sys, ai, op[1], len(segs)))
                for (bid, blen) in segs:
                    if blen < 0:
                        raise ValErr("call %d (%s) arg %d: iov %d segment has negative len %d"
                                     % (idx, c.sys, ai, op[1], blen))
                    if bid < 0 or bid not in bufids:
                        raise ValErr("call %d (%s) arg %d: iov %d segment buf %d does not exist"
                                     % (idx, c.sys, ai, op[1], bid))
                    if bufsize[bid] < blen:
                        raise ValErr("call %d (%s) arg %d: iov %d segment buf %d is %d bytes, need %d"
                                     % (idx, c.sys, ai, op[1], bid, bufsize[bid], blen))
                # the count operand must be an IMM equal to the segment count
                cnt = None
                for (o2, s2) in zip(c.args, specs):
                    if s2.get("name") == countarg and o2[0] == IMM:
                        cnt = o2[1]
                        break
                if cnt != len(segs):
                    raise ValErr("call %d (%s) arg %d: iov %d has %d segments but %s=%r"
                                 % (idx, c.sys, ai, op[1], len(segs), countarg, cnt))
            elif kind == NIL:
                if akind == "ptr" and not spec.get("optional"):
                    raise ValErr("call %d (%s) arg %d: NULL for a non-optional pointer %s"
                                 % (idx, c.sys, ai, spec.get("name")))
            elif kind == SLOT:
                # A baseline-relative descriptor number: only for a scalar arg
                # (an fd, or fcntl's F_DUPFD floor), never a pointer; the ordinal
                # must be a real non-negative index.
                if akind != "scalar":
                    raise ValErr("call %d (%s) arg %d: slot for a non-scalar arg %s"
                                 % (idx, c.sys, ai, spec.get("name")))
                if op[1] < 0 or (op[1] & SLOT_ORD_MASK) >= 32:
                    raise ValErr("call %d (%s) arg %d: slot %d has no ordinal in 0..31"
                                 % (idx, c.sys, ai, op[1]))
            elif kind == BAD:
                # A deliberately invalid pointer stands only for a pointer arg.
                if akind != "ptr":
                    raise ValErr("call %d (%s) arg %d: bad pointer for a non-ptr arg %s"
                                 % (idx, c.sys, ai, spec.get("name")))
            elif kind == IMM:
                # An immediate stands in only for a scalar (by-value) argument; a
                # raw integer where the ABI wants a pointer is a kind error
                # (pointers are buf/path/null/iov in this IR).
                if akind == "ptr":
                    raise ValErr("call %d (%s) arg %d: imm for a pointer arg %s (use buf/path/null/iov)"
                                 % (idx, c.sys, ai, spec.get("name")))
            else:
                raise ValErr("call %d (%s) arg %d: bad operand kind %r" % (idx, c.sys, ai, kind))

        # resource creation: a successful (static) create binds its handles LIVE.
        if c.bind is not None:
            rtype, hs = c.bind
            for h in hs:
                hstate[h] = LIVE
                created_by[h] = idx
        # resource release: whatever res-typed handle this call releases goes RELEASED.
        for rel in sc.get("resources", {}).get("releases", []):
            for (op, spec) in zip(c.args, specs):
                if op[0] == RES and spec.get("res") == rel:
                    hstate[op[1]] = RELEASED

    return {"calls": len(p.calls), "dep_edges": dep_edges}


def _resolve_variant(spec, call, specs):
    """For a `variant` arg, return the concrete shape dict its selector selects,
    or None when the selector operand is not a decidable immediate or its value
    matches no known case. Matching is by the case's `sel_value` (the numeric
    constant, validated against the kernel by check_abi)."""
    sel_name = spec.get("selector")
    sel_val = None
    for (op, s) in zip(call.args, specs):
        if s.get("name") == sel_name and op[0] == IMM:
            sel_val = op[1]
            break
    if sel_val is None:
        return None
    for shape in spec.get("variants", {}).values():
        if shape.get("sel_value") == sel_val:
            return shape
    return None


def _min_size(spec, call, prog, specs):
    """Minimum buffer size required by a pointer arg's size rule, when it is
    knowable statically (len:ARG, struct:NAME, fixed:N). None = unconstrained."""
    sz = spec.get("size", "")
    if sz.startswith("fixed:"):
        return int(sz.split(":")[1])
    if sz.startswith("struct:"):
        name = sz.split(":")[1]
        sd = prog_abi_struct_size(name)
        return sd
    if sz.startswith("len:"):
        argname = sz.split(":")[1]
        for (op, s) in zip(call.args, specs):
            if s.get("name") == argname and op[0] == IMM:
                return op[1]
    return None


# struct sizes used by _min_size; small, kept here to avoid importing Abi state.
# termios is MAKH's 12-byte {uint32_t c_lflag; uint8_t c_cc[8]} (tty.h), NOT
# Linux's 60-byte struct. selftest.py cross-checks every size that also appears
# in the ABI struct_defs, so this table cannot drift from the inventory.
_STRUCT_SIZES = {"stat": 144, "timespec": 16, "timeval": 16, "rlimit": 16,
                 "rlimit64": 16, "sigaction": 32, "utsname": 390, "iovec": 16,
                 "termios": 12, "makh_ifinfo": 64}


def prog_abi_struct_size(name):
    return _STRUCT_SIZES.get(name)


# ---- deterministic PRNG (portable, matches the guest if needed later) -----
class Lcg:
    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFFFFFFFFFF

    def next(self):
        # Numerical Recipes LCG constants (64-bit)
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        return self.s >> 33

    def below(self, n):
        return self.next() % n if n else 0

    def bytes(self, n):
        return bytes(self.next() & 0xFF for _ in range(n))
