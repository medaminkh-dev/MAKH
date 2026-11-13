#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U8 — structure-aware mutation of IR programs.
#
# Every operator edits a u3.Program; `mutant()` stacks 1..3 of them, then
#   rebalance  -> append the close/munmap of every handle left live, so the fd
#                 baseline guard (U5.2) does not flag the edit as a leak;
#   validate   -> the U3 validator (operand kinds, creator-before-use);
#   annotate   -> fresh expectations from the U5.3 deriver; a program it cannot
#                 fully predict is DROPPED (no blocking read, no SIGPIPE write,
#                 no undeclared allocation), and counted against the valid ratio.
#
# Value pools are per (syscall, argument) and limited to what the deriver models,
# so a mutant's expectation stays a statement about the contract.

import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "fuzz", "u7"))
import minimize as M  # noqa: E402

u3 = M.u3
IMM, RES, BUF, PATH, SLOT, BAD = u3.IMM, u3.RES, u3.BUF, u3.PATH, u3.SLOT, u3.BAD
MAX_BUFS, MAX_PATHS, MAX_H, MAX_CALLS = 8, 4, 34, 70

HI = 1 << 32
FD_POOL = [(IMM, -1), (IMM, 32), (IMM, 0x7fffffff), (IMM, 1), (IMM, 2)] + \
          [(SLOT, k) for k in range(0, 12, 3)] + [(SLOT, 2 | HI)]
POOLS = {
    ("close", 0): FD_POOL,
    ("dup2", 0): FD_POOL,
    ("dup2", 1): FD_POOL,
    ("fcntl", 0): FD_POOL,
    ("fcntl", 1): [(IMM, v) for v in (0, 1, 2, 3, 4, 999, -1)],
    ("fcntl", 2): [(IMM, v) for v in (-1, 0, 1, 2, 3, 31, 32, 0x7fffffff, 0x80000000,
                                     0x400, 0x800, 0xC03)] + [(SLOT, 0), (SLOT, 5)],
    ("lseek", 1): [(IMM, v) for v in (0, 1, -1, 4095, 4096, 12288, 274432, 0x7fffffff)],
    ("lseek", 2): [(IMM, v) for v in (0, 1, 2)],
    ("arch_prctl", 0): [(IMM, v) for v in (0x1001, 0x1002, 0x1003, 0x1004, 0)],
    ("socket", 0): [(IMM, v) for v in (2, 1, 10, 99)],
    ("mprotect", 2): [(IMM, v) for v in (0, 1, 3)],
}
OPEN_FLAGS = {"/bin/hello": [0, 0x400, 0x800, 0x80000],          # read-only file
              "/mnt/ufz": [0x242, 0x42, 0x2, 0x442]}              # the ext2 scratch file
BAD_PTRS = [0, -2147483648, 0x0000800000000000, 0x1000]


def _creator_index(prog, h, before):
    for i in range(before - 1, -1, -1):
        c = prog.calls[i]
        if c.bind and h in c.bind[1]:
            return i
    return None


def rebalance(prog):
    """Append close/munmap for every handle still live at the end."""
    live = {}
    for c in prog.calls:
        if c.bind:
            for h in c.bind[1]:
                live[h] = True
        if c.sys in ("close", "munmap") and c.args and c.args[0][0] == RES:
            live[c.args[0][1]] = False
    for h in sorted((h for h, v in live.items() if v), reverse=True):
        if prog.handles[h] == "fd":
            prog.calls.append(u3.Call("close", [(RES, h)]))
        elif prog.handles[h] == "addr":
            prog.calls.append(u3.Call("munmap", [(RES, h), (IMM, 4096)]))
    return prog


def _model_end(prog):
    """The deriver's model after the whole program (None if unpredictable)."""
    m = M.derive.Model(prog, M.abi())
    try:
        for c in prog.calls:
            m.predict(c)
    except M.derive.Unknown:
        return None
    return m


def finish(prog):
    """A runnable mutant (balanced, valid, fully annotated) or None.

    Balancing is done twice: by handle (rebalance) and then SEMANTICALLY, from
    the deriver's end state — an edit can create an fd no handle names (e.g. a
    value mutation turning fcntl F_GETFL into F_DUPFD), so every ordinal the model
    still holds open is closed by slot. The result must end with nothing open."""
    if not prog.calls or len(prog.calls) > MAX_CALLS or len(prog.handles) > MAX_H:
        return None
    for c in prog.calls:
        c.expect = None
    rebalance(prog)
    if not M.valid(prog):
        return None
    m = _model_end(prog)
    if m is None:
        return None
    for o in sorted(m.ords, reverse=True):
        prog.calls.append(u3.Call("close", [(SLOT, o)]))
    if len(prog.calls) > MAX_CALLS or not M.valid(prog):
        return None
    out = M.annotate(prog)
    if out is None:
        return None
    end = _model_end(out)
    if end is None or end.ords or end.addrs:
        return None
    return out


# ---- operators: each returns a new program or None ----------------------------------
def op_value(prog, rng):
    """Integer boundary / flag / length / offset, from per-argument pools."""
    sites = []
    for i, c in enumerate(prog.calls):
        for ai, op in enumerate(c.args):
            if (c.sys, ai) in POOLS:
                sites.append((i, ai, POOLS[(c.sys, ai)]))
        if c.sys in ("open", "openat"):
            pi = 0 if c.sys == "open" else 1
            path = dict(prog.paths).get(c.args[pi][1]) if c.args[pi][0] == PATH else None
            if path in OPEN_FLAGS:
                sites.append((i, pi + 1, [(IMM, v) for v in OPEN_FLAGS[path]]))
        if c.sys in ("read", "write") and len(c.args) == 3 and c.args[1][0] == BUF:
            size = dict((b[0], b[1]) for b in prog.buffers).get(c.args[1][1], 1)
            sites.append((i, 2, [(IMM, v) for v in sorted({0, 1, max(1, size // 2), size})]))
    if not sites:
        return None
    i, ai, pool = sites[rng.randrange(len(sites))]
    p = copy.deepcopy(prog)
    args = list(p.calls[i].args)
    args[ai] = pool[rng.randrange(len(pool))]
    p.calls[i].args = args
    return p


def op_delete(prog, rng):
    if len(prog.calls) < 2:
        return None
    return M.delete_calls(prog, [rng.randrange(len(prog.calls))])


def op_duplicate(prog, rng):
    p = copy.deepcopy(prog)
    i = rng.randrange(len(p.calls))
    c = copy.deepcopy(p.calls[i])
    if c.bind:
        new = []
        for h in c.bind[1]:
            p.handles.append(p.handles[h])
            new.append(len(p.handles) - 1)
        c.bind = (c.bind[0], new)
    p.calls.insert(i + 1, c)
    return p


def op_reorder(prog, rng):
    if len(prog.calls) < 2:
        return None
    p = copy.deepcopy(prog)
    i = rng.randrange(len(p.calls))
    c = p.calls.pop(i)
    p.calls.insert(rng.randrange(len(p.calls) + 1), c)
    return p


def op_alias(prog, rng):
    """Give a call a different live-or-dead handle of the same type."""
    sites = [(i, ai) for i, c in enumerate(prog.calls) for ai, op in enumerate(c.args)
             if op[0] == RES]
    if not sites:
        return None
    i, ai = sites[rng.randrange(len(sites))]
    want = prog.handles[prog.calls[i].args[ai][1]]
    cands = sorted({h for j in range(i) for h in (prog.calls[j].bind[1] if prog.calls[j].bind else [])
                    if prog.handles[h] == want} - {prog.calls[i].args[ai][1]})
    if not cands:
        return None
    p = copy.deepcopy(prog)
    args = list(p.calls[i].args)
    args[ai] = (RES, cands[rng.randrange(len(cands))])
    p.calls[i].args = args
    return p


def op_pointer(prog, rng):
    """A pipe/pipe2 output pointer at a boundary: null, kernel half, non-canonical,
    unmapped. The call can no longer create its handles."""
    sites = [i for i, c in enumerate(prog.calls) if c.sys in ("pipe", "pipe2")
             and c.args and c.args[0][0] == BUF]
    if not sites:
        return None
    i = sites[rng.randrange(len(sites))]
    hs = list(prog.calls[i].bind[1]) if prog.calls[i].bind else []
    users = [j for j, c in enumerate(prog.calls)
             if j != i and any(op[0] == RES and op[1] in hs for op in c.args)]
    p = M.delete_calls(prog, users) if users else copy.deepcopy(prog)
    k = sum(1 for j in users if j < i)
    args = list(p.calls[i - k].args)
    args[0] = (BAD, BAD_PTRS[rng.randrange(len(BAD_PTRS))])
    p.calls[i - k].args = args
    p.calls[i - k].bind = None
    return p


def op_bytes(prog, rng):
    filled = [k for k, b in enumerate(prog.buffers) if any(b[2])]
    if not filled:
        return None
    p = copy.deepcopy(prog)
    k = filled[rng.randrange(len(filled))]
    bid, size, data = p.buffers[k]
    d = bytearray(data)
    for _ in range(1 + rng.randrange(4)):
        d[rng.randrange(len(d))] ^= 1 << rng.randrange(8)
    p.buffers[k] = (bid, size, bytes(d))
    return p


def graft(prog, donor, j, rng):
    """Insert donor call j with every call that creates a handle it uses (the
    closure, in donor order) at a random position, remapping handles, buffers
    and paths. None if a capacity would be exceeded."""
    need = {j}
    todo = [j]
    while todo:
        x = todo.pop()
        for op in donor.calls[x].args:
            if op[0] == RES:
                ci = _creator_index(donor, op[1], x)
                if ci is not None and ci not in need:
                    need.add(ci)
                    todo.append(ci)
    p = copy.deepcopy(prog)
    hmap, bmap, pmap = {}, {}, {}
    dpaths = dict(donor.paths)
    bufs = dict((b[0], b) for b in donor.buffers)
    block = []
    for x in sorted(need):
        c = copy.deepcopy(donor.calls[x])
        args = []
        for op in c.args:
            if op[0] == RES:
                if op[1] not in hmap:
                    return None                    # uses a handle created outside the block
                args.append((RES, hmap[op[1]]))
            elif op[0] == BUF:
                if op[1] not in bmap:
                    if len(p.buffers) >= MAX_BUFS:
                        return None
                    nb = len(p.buffers)
                    _, size, data = bufs[op[1]]
                    p.buffers.append((nb, size, data))
                    bmap[op[1]] = nb
                args.append((BUF, bmap[op[1]]))
            elif op[0] == PATH:
                s = dpaths[op[1]]
                if op[1] not in pmap:
                    mine = [pid for pid, ps in p.paths if ps == s]
                    if mine:
                        pmap[op[1]] = mine[0]
                    else:
                        if len(p.paths) >= MAX_PATHS:
                            return None
                        pmap[op[1]] = len(p.paths)
                        p.paths.append((len(p.paths), s))
                args.append((PATH, pmap[op[1]]))
            else:
                args.append(op)
        c.args = args
        if c.bind:
            new = []
            for h in c.bind[1]:
                p.handles.append(donor.handles[h])
                hmap[h] = len(p.handles) - 1
                new.append(hmap[h])
            c.bind = (c.bind[0], new)
        block.append(c)
    # Insert before the trailing balance closes, so the block's own uses run.
    pos = rng.randrange(len(p.calls) + 1)
    p.calls[pos:pos] = block
    if p.need is None and donor.need is not None:
        p.need = donor.need
    return p


def op_insert(prog, rng, donor):
    if donor is None or not donor.calls:
        return None
    return graft(prog, donor, rng.randrange(len(donor.calls)), rng)


OPS = [("value", op_value), ("delete", op_delete), ("duplicate", op_duplicate),
       ("reorder", op_reorder), ("alias", op_alias), ("pointer", op_pointer),
       ("bytes", op_bytes), ("insert", None), ("splice", None)]


def mutant(parent, rng, donor_fresh, donor_corpus, max_ops=3):
    """Stack 1..max_ops operators. Returns (program or None, [op names])."""
    p = copy.deepcopy(parent)
    for c in p.calls:
        c.expect = None
    names = []
    for _ in range(1 + rng.randrange(max_ops)):
        name, fn = OPS[rng.randrange(len(OPS))]
        if name == "insert":
            q = op_insert(p, rng, donor_fresh)
        elif name == "splice":
            q = op_insert(p, rng, donor_corpus)
        else:
            q = fn(p, rng)
        if q is not None:
            p = q
            names.append(name)
    if not names:
        return None, names
    return finish(p), names
