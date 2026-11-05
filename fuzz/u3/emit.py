#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U3 — emit the guest program set as a C header.
#
# The host IR is the source; the guest (/bin/ufuzz U3 mode) executes a baked
# set. This tool produces user/ufuzz_u3_programs.h from a curated set: a few
# deterministic generated programs plus hand-crafted feature templates (pipe
# round-trip, mmap success, mmap failure). Syscall numbers come from the U0 ABI
# inventory, so the header cannot drift from the kernel's numbers.
#
#   python3 fuzz/u3/emit.py            # regenerate the header
#   python3 fuzz/u3/emit.py --check    # fail if the committed header is stale

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import u3
import gen

ROOT = u3.ROOT
HDR = os.path.join(ROOT, "user", "ufuzz_u3_programs.h")

# expect modes (must match ufuzz.c)
ANY, NONNEG, NEG, EXACT = 0, 1, 2, 3
# bind kinds (must match ufuzz.c)
B_NONE, B_FD, B_ADDR, B_PIPE = 0, 1, 2, 3


def _bind_of(call):
    if call.bind is None:
        return (B_NONE, 0, 0, 0)
    rtype, hs = call.bind
    if rtype == "addr":
        return (B_ADDR, hs[0], 0, 0)
    if rtype == "fd" and len(hs) == 2:
        # pipe: the two ends come from the buffer the pipe wrote
        bufid = next(op[1] for op in call.args if op[0] == u3.BUF)
        return (B_PIPE, hs[0], hs[1], bufid)
    return (B_FD, hs[0], 0, 0)


def prog_from_gen(name, seed, steps=6):
    p = gen.generate(u3.Abi(), seed, steps)
    calls = []
    for c in p.calls:
        calls.append({"sys": c.sys, "args": c.args, "bind": _bind_of(c),
                      "expect": (ANY, 0)})
    return {"name": name, "expect_fault": False, "replay": True,
            "handles": p.handles, "bufs": [b[2] for b in p.buffers],
            "paths": [pp[1] for pp in p.paths], "calls": calls, "data_check": None}


def template_pipe_roundtrip():
    msg = bytes(range(8))
    return {
        "name": "pipe_roundtrip", "expect_fault": False, "replay": True,
        "handles": ["fd", "fd"],
        "bufs": [bytes(8), msg, bytes(8)],   # 0: pipefd[2]; 1: write src; 2: read dst
        "paths": [],
        "calls": [
            {"sys": "pipe",  "args": [(u3.BUF, 0)], "bind": (B_PIPE, 0, 1, 0), "expect": (NONNEG, 0)},
            {"sys": "write", "args": [(u3.RES, 1), (u3.BUF, 1), (u3.IMM, 8)], "bind": (B_NONE, 0, 0, 0), "expect": (EXACT, 8)},
            {"sys": "read",  "args": [(u3.RES, 0), (u3.BUF, 2), (u3.IMM, 8)], "bind": (B_NONE, 0, 0, 0), "expect": (EXACT, 8)},
            {"sys": "close", "args": [(u3.RES, 0)], "bind": (B_NONE, 0, 0, 0), "expect": (NONNEG, 0)},
            {"sys": "close", "args": [(u3.RES, 1)], "bind": (B_NONE, 0, 0, 0), "expect": (NONNEG, 0)},
        ],
        "data_check": (2, 1, 8),   # read dst == write src for 8 bytes
    }


def template_mmap_success():
    PROT_RW, MAP = 0x3, 0x22
    return {
        "name": "mmap_success", "expect_fault": False, "replay": True,
        "handles": ["addr"], "bufs": [], "paths": [],
        "calls": [
            {"sys": "mmap",     "args": [(u3.IMM, 0), (u3.IMM, 4096), (u3.IMM, PROT_RW), (u3.IMM, MAP)], "bind": (B_ADDR, 0, 0, 0), "expect": (NONNEG, 0)},
            {"sys": "mprotect", "args": [(u3.RES, 0), (u3.IMM, 4096), (u3.IMM, 0x1)], "bind": (B_NONE, 0, 0, 0), "expect": (NONNEG, 0)},
            {"sys": "munmap",   "args": [(u3.RES, 0), (u3.IMM, 4096)], "bind": (B_NONE, 0, 0, 0), "expect": (NONNEG, 0)},
        ],
        "data_check": None,
    }


def template_mmap_fail():
    # A length so large uvm_mmap must reject it -> negative return -> the addr
    # handle is FAILED (success-gating), and the following munmap runs against
    # that failed value without crashing the controller.
    PROT_RW, MAP = 0x3, 0x22
    HUGE = (1 << 46)
    return {
        "name": "mmap_fail", "expect_fault": False, "replay": True,
        "handles": ["addr"], "bufs": [], "paths": [],
        "calls": [
            {"sys": "mmap",   "args": [(u3.IMM, 0), (u3.IMM, HUGE), (u3.IMM, PROT_RW), (u3.IMM, MAP)], "bind": (B_ADDR, 0, 0, 0), "expect": (NEG, 0)},
            {"sys": "munmap", "args": [(u3.RES, 0), (u3.IMM, 4096)], "bind": (B_NONE, 0, 0, 0), "expect": (ANY, 0)},
        ],
        "data_check": None,
    }


def program_set():
    return ([prog_from_gen("gen%d" % s, s) for s in range(3)] +
            [template_pipe_roundtrip(), template_mmap_success(), template_mmap_fail()])


def _op_c(op):
    k = op[0]
    if k == u3.IMM:
        return "{U3_IMM, %d}" % op[1]
    if k == u3.RES:
        return "{U3_RES, %d}" % op[1]
    if k == u3.NIL:
        return "{U3_NIL, 0}"
    if k == u3.BUF:
        return "{U3_BUF, %d}" % op[1]
    if k == u3.PATH:
        return "{U3_PATH, %d}" % op[1]
    raise ValueError(op)


def render(progs, abi):
    nr = {s["name"]: s["nr"] for s in abi.data["syscalls"]}
    L = []
    L.append("/* SPDX-License-Identifier: AGPL-3.0-only */")
    L.append("/* Copyright (C) 2026 Amine Khemissi */")
    L.append("/* GENERATED by fuzz/u3/emit.py — do not edit. Regenerate:")
    L.append(" *   python3 fuzz/u3/emit.py   (and `make u3-check` / CI verifies freshness) */")
    L.append("#ifndef MAKHOS_UFUZZ_U3_PROGRAMS_H")
    L.append("#define MAKHOS_UFUZZ_U3_PROGRAMS_H")
    L.append("")
    L.append("#define U3_IMM 0\n#define U3_RES 1\n#define U3_NIL 2\n#define U3_BUF 3\n#define U3_PATH 4")
    L.append("#define U3_BIND_NONE 0\n#define U3_BIND_FD 1\n#define U3_BIND_ADDR 2\n#define U3_BIND_PIPE 3")
    L.append("#define U3_ANY 0\n#define U3_NONNEG 1\n#define U3_NEG 2\n#define U3_EXACT 3")
    L.append("")
    L.append("typedef struct { int kind; long val; } u3_arg;")
    L.append("typedef struct { long sysno; int nargs; u3_arg args[6];")
    L.append("                 int bind, bind_h0, bind_h1, bind_buf;")
    L.append("                 int expect_mode; long expect_val; } u3_call;")
    L.append("typedef struct { const unsigned char* data; int len; } u3_buf;")
    L.append("typedef struct { const char* name; int expect_fault; int replay; int nhandles;")
    L.append("                 const u3_buf* bufs; int nbufs;")
    L.append("                 const char* const* paths; int npaths;")
    L.append("                 const u3_call* calls; int ncalls;")
    L.append("                 int dc_dst, dc_src, dc_len; } u3_program;")
    L.append("")
    for i, pg in enumerate(progs):
        # buffers
        for bi, b in enumerate(pg["bufs"]):
            arr = ", ".join(str(x) for x in b) if b else ""
            L.append("static const unsigned char u3_%d_buf%d[] = {%s};" % (i, bi, arr))
        if pg["bufs"]:
            L.append("static const u3_buf u3_%d_bufs[] = {%s};"
                     % (i, ", ".join("{u3_%d_buf%d, %d}" % (i, bi, len(b)) for bi, b in enumerate(pg["bufs"]))))
        # paths
        if pg["paths"]:
            L.append("static const char* const u3_%d_paths[] = {%s};"
                     % (i, ", ".join('"%s"' % p for p in pg["paths"])))
        # calls
        L.append("static const u3_call u3_%d_calls[] = {" % i)
        for c in pg["calls"]:
            args = ", ".join(_op_c(op) for op in c["args"]) or "{0, 0}"
            bind, h0, h1, bb = c["bind"]
            bindname = {B_NONE: "U3_BIND_NONE", B_FD: "U3_BIND_FD", B_ADDR: "U3_BIND_ADDR", B_PIPE: "U3_BIND_PIPE"}[bind]
            em = {ANY: "U3_ANY", NONNEG: "U3_NONNEG", NEG: "U3_NEG", EXACT: "U3_EXACT"}[c["expect"][0]]
            L.append("  { %d, %d, {%s}, %s, %d, %d, %d, %s, %d },"
                     % (nr[c["sys"]], len(c["args"]), args, bindname, h0, h1, bb, em, c["expect"][1]))
        L.append("};")
    L.append("")
    L.append("static const u3_program U3_PROGRAMS[] = {")
    for i, pg in enumerate(progs):
        dc = pg["data_check"]
        dcs = ("%d, %d, %d" % dc) if dc else "-1, -1, 0"
        bufs = ("u3_%d_bufs, %d" % (i, len(pg["bufs"]))) if pg["bufs"] else "0, 0"
        paths = ("u3_%d_paths, %d" % (i, len(pg["paths"]))) if pg["paths"] else "0, 0"
        L.append('  { "%s", %d, %d, %d, %s, %s, u3_%d_calls, %d, %s },'
                 % (pg["name"], 1 if pg["expect_fault"] else 0, 1 if pg["replay"] else 0,
                    len(pg["handles"]), bufs, paths, i, len(pg["calls"]), dcs))
    L.append("};")
    L.append("static const int U3_NPROGRAMS = %d;" % len(progs))
    L.append("")
    L.append("#endif /* MAKHOS_UFUZZ_U3_PROGRAMS_H */")
    return "\n".join(L) + "\n"


def main():
    abi = u3.Abi()
    text = render(program_set(), abi)
    if "--check" in sys.argv[1:]:
        cur = open(HDR).read() if os.path.exists(HDR) else ""
        if cur != text:
            print("U3 EMIT: STALE — regenerate with `python3 fuzz/u3/emit.py`", file=sys.stderr)
            sys.exit(1)
        print("U3 EMIT: header is up to date")
    else:
        with open(HDR, "w") as f:
            f.write(text)
        print("wrote %s (%d programs)" % (os.path.relpath(HDR, ROOT), len(program_set())))


if __name__ == "__main__":
    main()
