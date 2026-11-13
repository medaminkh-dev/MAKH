#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U6 — crash classification, triage and artifacts (host side).
#
# Reads the serial logs of test runs (tools/run_tests.py output: the kernel log
# plus the `qemu:` command and the `=== qemu exit status` / `=== RESULT` lines),
# joins the machine-readable `UFZ1` records the kernel, the KTEST campaigns and
# /bin/ufuzz print, classifies every event, deduplicates by signature and
# writes one artifact directory per unique finding.
#
#   ufuzz_triage.py LOG... [--out DIR] [--kernel ELF] [--u4gen BIN]
#                          [--check] [--fail-on-findings] [--quiet]
#
#   --check             fail if any artifact lacks a required field
#   --fail-on-findings  fail if the logs contain any finding (canaries and
#                       negative controls that fail as designed are not findings;
#                       a canary that PASSES is one)
#
# Records (one line each, integers and fixed tokens only):
#   UFZ1 build id=<commit hash>[-dirty]
#   UFZ1 tcb n= src=cov|selfcheck a= b= c=          testcase begins (KTEST)
#   UFZ1 tc n= cpid= status= expect=ok|fail          testcase ends (KTEST)
#   UFZ1 res src= cpid= wpid= a= b= c= outcome= status=   non-clean run (controller)
#   UFZ1 diag pid= pass= cls= what= diag= call= [nr= emode= ret= want=]  (worker)
#   UFZ1 trace pid= pass= n= v=r0,r1,...             (worker)
#   UFZ1 ufault pid= vec= err= rip= cr2=             ring-3 fault (kernel)
#   UFZ1 drift dpmm= dheap=                          batch not net-zero (KTEST)
#   UFZ1 edges n= new=                               coverage delta (KTEST)
#   UFZ1 replay ...                                  replay suite
#
# Nothing taken from a log is ever used in a path or a shell command: artifact
# directories are named by a hash of the signature, replay commands are built
# from parsed integers only.

import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))

CLASSES = ("EXPECTED_ERRNO", "USER_FAULT", "TARGET_CRASH", "KERNEL_EXCEPTION",
           "KERNEL_PANIC", "KERNEL_HANG", "TIMEOUT", "ORACLE_VIOLATION",
           "RESOURCE_LEAK", "INVARIANT_FAILURE", "HARNESS_FAILURE", "INFRA_FAILURE")

REQUIRED = ("classification", "subkind", "reason", "confidence", "signature",
            "build_id", "abi_schema", "prng", "qemu", "budget_ms", "inputs",
            "replay", "occurrences", "first_log", "log_excerpt")

# Worker self-check class codes (user/ufuzz.c U3X_*) and controller outcomes.
WORKER_CLS = {101: "expectation", 102: "data-check", 103: "replay", 104: "digest",
              105: "capacity", 106: "unknown", 107: "contract", 108: "isolation"}
COV_OUTCOME = {0: "OK", 2: "USER_FAULT", 3: "TIMEOUT", 4: "OTHER"}

REC = re.compile(r"^UFZ1 (\w+)(.*)$")
KV = re.compile(r"(\w+)=(\S*)")


def kv(rest):
    return dict(KV.findall(rest))


def ival(d, k, default=None):
    try:
        return int(d[k], 0)
    except (KeyError, ValueError):
        return default


def _abi_schema():
    try:
        with open(os.path.join(ROOT, "fuzz", "abi", "makh_syscalls.json")) as f:
            d = json.load(f)
        return "%s v%s" % (d.get("schema"), d.get("abi_version"))
    except (OSError, ValueError):
        return "unavailable"


def _budget_ms():
    try:
        with open(os.path.join(ROOT, "user", "ufuzz.c")) as f:
            m = re.search(r"#define\s+BUDGET_MS\s+(\d+)", f.read())
        return int(m.group(1)) if m else "unavailable"
    except OSError:
        return "unavailable"


def _syscall_names():
    try:
        with open(os.path.join(ROOT, "fuzz", "abi", "makh_syscalls.json")) as f:
            d = json.load(f)
        return {s["nr"]: s["name"] for s in d["syscalls"]}
    except (OSError, ValueError):
        return {}


def _errno_names():
    out = {}
    try:
        with open(os.path.join(ROOT, "kernel", "include", "errno.h")) as f:
            for line in f:
                m = re.match(r"\s*#define\s+(E[A-Z0-9]+)\s+(\d+)", line)
                if m:
                    out[int(m.group(2))] = m.group(1)
    except OSError:
        pass
    return out


SYSNAME = _syscall_names()
ERRNAME = _errno_names()


# ---- symbolization ------------------------------------------------------------
class Symbols:
    """Resolve kernel addresses with `nm` on the kernel ELF — only if that ELF
    carries the same build id as the log (else addresses stay raw)."""

    def __init__(self, elf, build_id):
        self.syms = []
        if not elf or not os.path.exists(elf) or not build_id:
            return
        try:
            with open(elf, "rb") as f:
                if build_id.encode() not in f.read():
                    return
            out = subprocess.run(["nm", "-n", elf], capture_output=True, text=True,
                                 check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            return
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tTwW":
                self.syms.append((int(parts[0], 16), parts[2]))

    def name(self, addr):
        if not self.syms:
            return "0x%x" % addr
        best = None
        for a, n in self.syms:
            if a > addr:
                break
            best = (a, n)
        return "%s+0x%x" % (best[1], addr - best[0]) if best else "0x%x" % addr

    def func(self, addr):
        return self.name(addr).split("+")[0]


# ---- one log -------------------------------------------------------------------
class LogState:
    def __init__(self, path):
        self.path = path
        self.lines = []
        self.build_id = None
        self.qemu = None
        self.exit = None              # int, "TIMEOUT", or None (no exit line)
        self.result = None
        self.tcb = {}                 # n -> dict(src,a,b,c,campaign,line)
        self.tc = {}                  # n -> dict(cpid,status,expect)
        self.res = {}                 # cpid -> dict
        self.diag = {}                # wpid -> dict
        self.trace = {}               # wpid -> dict
        self.ufault = {}              # pid -> dict
        self.drift = []               # (campaign, dict, line)
        self.edges = {}               # n -> new edges
        self.ktest_fail = []          # (campaign, file:line, expr, line)
        self.kexc = None              # kernel exception block
        self.panic = None             # kernel panic block
        self.open_tc = None           # last tcb without an end, at the point of a crash
        self.infra = []               # infrastructure problems


def parse_log(path):
    st = LogState(path)
    with open(path, errors="replace") as f:
        st.lines = f.read().splitlines()
    campaign = None
    last_open = None
    i = 0
    L = st.lines
    while i < len(L):
        line = L[i]
        m = re.match(r"^\[ RUN  \] (\S+)", line)
        if m:
            campaign = m.group(1)
        if line.startswith("qemu: "):
            st.qemu = line[6:].strip()
        m = re.match(r"^=== qemu exit status: (\S+) ===", line)
        if m:
            st.exit = int(m.group(1)) if m.group(1).lstrip("-").isdigit() else m.group(1)
        if line.startswith("=== RESULT: "):
            st.result = line
        if re.match(r"^make: \*\*\*", line):
            st.infra.append((i, line))
        m = re.match(r"^\s+FAIL (\S+:\d+): (.*)$", line)
        if m:
            st.ktest_fail.append((campaign, m.group(1), m.group(2), i))
        m = REC.match(line)
        if m:
            kind, d = m.group(1), kv(m.group(2))
            if kind == "build":
                st.build_id = d.get("id")
            elif kind == "tcb":
                n = ival(d, "n")
                st.tcb[n] = dict(src=d.get("src"), a=ival(d, "a"), b=ival(d, "b"),
                                 c=ival(d, "c"), campaign=campaign, line=i)
                last_open = n
            elif kind == "tc":
                n = ival(d, "n")
                st.tc[n] = dict(cpid=ival(d, "cpid"), status=ival(d, "status"),
                                expect=d.get("expect"), line=i)
                if last_open == n:
                    last_open = None
            elif kind == "res":
                d["line"] = i
                st.res[ival(d, "cpid")] = d
            elif kind == "diag":
                d["line"] = i
                st.diag[ival(d, "pid")] = d
            elif kind == "trace":
                st.trace[ival(d, "pid")] = d
            elif kind == "ufault":
                d["line"] = i
                st.ufault[ival(d, "pid")] = d
            elif kind == "drift":
                st.drift.append((campaign, d, i))
            elif kind == "edges":
                st.edges[ival(d, "n")] = ival(d, "new")
        if line.startswith("!!! EXCEPTION CAUGHT !!!") and st.kexc is None:
            st.kexc = _parse_exception(L, i)
            st.open_tc = last_open
            st.kexc["campaign"] = campaign
        if line.startswith("*** KERNEL PANIC ***") and st.panic is None:
            st.panic = _parse_panic(L, i)
            st.open_tc = last_open
            st.panic["campaign"] = campaign
        i += 1
    if st.kexc is None and st.panic is None and st.exit == "TIMEOUT":
        st.open_tc = last_open
    st.last_campaign = campaign
    return st


def _parse_exception(L, i):
    e = dict(line=i, vector=None, err=None, rip=None, cr2=None, frames=[], thread=None)
    for j in range(i + 1, min(i + 40, len(L))):
        s = L[j]
        m = re.search(r"\(vector (\d+)\)", s)
        if m and e["vector"] is None:
            e["vector"] = int(m.group(1))
            e["name"] = s.split(":", 1)[1].split("(")[0].strip() if ":" in s else ""
        m = re.match(r"^Error code: (0x[0-9a-fA-F]+)", s)
        if m:
            e["err"] = int(m.group(1), 16)
        m = re.match(r"^RIP=(0x[0-9a-fA-F]+)", s)
        if m:
            e["rip"] = int(m.group(1), 16)
        m = re.match(r"^Page fault address: (0x[0-9a-fA-F]+)", s)
        if m:
            e["cr2"] = int(m.group(1), 16)
        m = re.match(r"^Thread: '([^']*)' pid (\d+)", s)
        if m:
            e["thread"] = (m.group(1), int(m.group(2)))
        m = re.match(r"^\s+#[-0-9]+\s+(0x[0-9a-fA-F]+)", s)
        if m:
            e["frames"].append(int(m.group(1), 16))
        if s.startswith("=== qemu exit status"):
            break
    return e


def _parse_panic(L, i):
    p = dict(line=i, message="", frames=[])
    for j in range(i + 1, min(i + 30, len(L))):
        s = L[j]
        if not p["message"] and s.strip() and not s.startswith("Backtrace"):
            p["message"] = s.strip()
        m = re.match(r"^\s+#[-0-9]+\s+(0x[0-9a-fA-F]+)", s)
        if m:
            p["frames"].append(int(m.group(1), 16))
        if s.startswith("=== qemu exit status"):
            break
    return p


# ---- classification -------------------------------------------------------------
def _errclass(v):
    if v is None:
        return "?"
    if v < 0:
        return ERRNAME.get(-v, "E%d" % -v)
    return "ret>=0"


def _cr2_class(cr2):
    if cr2 is None:
        return "n/a"
    if cr2 < 0x1000:
        return "null-page"
    if cr2 < 0x0000800000000000:
        return "user-half"
    return "kernel-half"


def _replay_cov(a, b, c):
    return "make test-suite SUITE=ufuzzreplay REPLAY=%d:%d:%d" % (a, b, c)


def _replay_ir(k):
    return ("python3 tools/u7_batch.py image <batch.ir> build/u7.img && make test-suite "
            "SUITE=ufuzzmin BATCH=3 EXT2=build/u7.img  (program %d of the batch)" % k)


def _program_ir(u4gen, a, b, c):
    if not u4gen or not os.path.exists(u4gen):
        return None
    try:
        out = subprocess.run([u4gen, str(b), "1", str(a), str(c)], capture_output=True,
                             text=True, check=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return out


# Selector-switched syscalls: which argument picks the operation. Read from the
# worker's diag record (a0/a1 = resolved arguments), so a signature depends on
# the log alone, never on whether a generator binary is at hand.
SELECTOR_ARG = {"fcntl": "a1", "ioctl": "a1", "arch_prctl": "a0"}


def _selector(sysn, diag):
    k = SELECTOR_ARG.get(sysn)
    if k is None or k not in diag:
        return None
    return "%s=%s" % ("cmd" if k == "a1" else "code", diag[k])


def classify_log(st, syms, u4gen):
    """Return (findings, expected, counts). A finding is a dict with the
    artifact fields plus `sigkey` (a tuple) used for dedup."""
    findings, expected = [], []
    counts = {"testcases": 0, "clean": 0, "expected_fail": 0, "expected_errno": 0}
    explained_campaigns = set()
    base = dict(build_id=st.build_id or "unknown", abi_schema=_abi_schema(),
                prng="lcg64 (6364136223846793005, 1442695040888963407) >>33",
                qemu=st.qemu or "unavailable", budget_ms=_budget_ms(),
                first_log=os.path.basename(st.path))

    def excerpt(lo, hi):
        lo = max(0, lo)
        return "\n".join(st.lines[lo:hi])

    def add(cls, sub, reason, conf, sigkey, inputs, replay, lo, hi, extra=None):
        f = dict(base)
        f.update(classification=cls, subkind=sub, reason=reason, confidence=conf,
                 sigkey=(cls,) + tuple(sigkey), inputs=inputs, replay=replay,
                 log_excerpt=excerpt(lo, hi), occurrences=1)
        if extra:
            f.update(extra)
        findings.append(f)
        if inputs.get("campaign"):
            explained_campaigns.add(inputs["campaign"])

    for n in sorted(k for k in st.tcb if k is not None):
        b = st.tcb[n]
        end = st.tc.get(n)
        counts["testcases"] += 1
        inputs = dict(campaign=b["campaign"], testcase=n, src=b["src"],
                      mode=b["a"], base=b["b"], mut=b["c"])
        if b["src"] == "selfcheck":
            inputs = dict(campaign=b["campaign"], testcase=n, src="selfcheck", kind=b["a"])
        elif b["src"] == "ir":                  # U7: batch candidate a, run b
            inputs = dict(campaign=b["campaign"], testcase=n, src="ir", candidate=b["a"],
                          run=b["b"])
        if n in st.edges:
            inputs["new_edges"] = st.edges[n]
        if end is None:
            continue                          # crash/hang inside: handled below
        status, exp = end["status"], end["expect"]
        if status == 0 and exp == "ok":
            counts["clean"] += 1
            if b["src"] == "cov" and u4gen:
                ir = _program_ir(u4gen, b["a"], b["b"], b["c"])
                if ir:
                    counts["expected_errno"] += len(re.findall(r" =exact:-\d+", ir))
            continue
        if status != 0 and exp == "fail":
            counts["expected_fail"] += 1
            expected.append(inputs)
            continue
        replay = (_replay_cov(b["a"], b["b"], b["c"]) if b["src"] == "cov"
                  else _replay_ir(b["a"]) if b["src"] == "ir"
                  else "make test-suite SUITE=ufuzz  (selfcheck kind %d)" % b["a"])
        if status == 0 and exp == "fail":
            add("HARNESS_FAILURE", "canary-passed",
                "a negative canary/control reported clean: the oracle it guards is blind",
                "high", (b["src"], b["a"]), inputs, replay, end["line"] - 3, end["line"] + 1)
            continue
        if status is None or status < 0:
            add("INFRA_FAILURE", "spawn", "the testcase could not be started", "medium",
                (b["src"],), inputs, replay, end["line"] - 2, end["line"] + 1)
            continue
        res = st.res.get(end["cpid"])
        wpid = ival(res, "wpid") if res else None
        diag = st.diag.get(wpid) if wpid is not None else None
        tr = st.trace.get(wpid) if wpid is not None else None
        uf = st.ufault.get(wpid) if wpid is not None else None
        lo = (res or {}).get("line", end["line"]) - 4
        extra = {}
        if tr:
            extra["trace"] = dict(pass_=ival(tr, "pass"), returns=tr.get("v", ""))
        if res:
            extra["worker_pid"] = wpid
        ir = _program_ir(u4gen, b["a"], b["b"], b["c"]) if b["src"] == "cov" else None
        if ir:
            extra["program_ir"] = ir
        outcome = COV_OUTCOME.get(status, "status-%d" % status)
        if outcome == "USER_FAULT" or (res and res.get("outcome") == "USER_FAULT"):
            if uf:
                vec, err, rip = ival(uf, "vec"), ival(uf, "err"), ival(uf, "rip")
                extra.update(fault=dict(vector=vec, err=err, rip=rip, cr2=ival(uf, "cr2")))
                add("USER_FAULT", "vector-%d" % vec,
                    "the worker took CPU exception %d in ring 3 (SIGSEGV)" % vec, "high",
                    (vec, err, rip), inputs, replay, lo, end["line"] + 1, extra)
            else:
                add("USER_FAULT", "unattributed", "SIGSEGV without a fault record", "low",
                    ("?",), inputs, replay, lo, end["line"] + 1, extra)
            continue
        if outcome == "TIMEOUT":
            add("TIMEOUT", "watchdog", "the worker exceeded its budget and was killed "
                "(not by itself evidence of a deadlock)", "medium",
                (b["src"], b["a"]), inputs, replay, lo, end["line"] + 1, extra)
            continue
        if diag is None:
            add("HARNESS_FAILURE", "no-diag", "non-clean run without a worker record",
                "low", (b["src"], status), inputs, replay, lo, end["line"] + 1, extra)
            continue
        cls = ival(diag, "cls")
        what = diag.get("what", "?")
        dcode = ival(diag, "diag", 0)
        call = ival(diag, "call")
        nr = ival(diag, "nr")
        extra["diag"] = {k: v for k, v in diag.items() if k != "line"}
        if cls in (101, 102, 103, 104, 107):
            sysn = SYSNAME.get(nr, "nr%s" % nr) if nr is not None else None
            ret = ival(diag, "ret")
            sel = _selector(sysn, diag)
            # The fault is (oracle, syscall, selector, kind of wrong result). How
            # the expectation was formed (exact vs baseline flags) is not part
            # of it: the same broken F_GETFL fails both kinds.
            # The required result's kind is part of the fault: "returned a value
            # where -EBADF was due" is not "returned the wrong value".
            sig = (what, sysn, _errclass(ret) if cls == 101 else None,
                   _errclass(ival(diag, "want")) if cls == 101 else None, sel)
            reason = "%s violated at call %s (%s%s)" % (what, call, sysn,
                                                        ", " + sel if sel else "")
            if cls == 101:
                reason += ": returned %s, contract requires %s" % (diag.get("ret"),
                                                                     diag.get("want"))
            add("ORACLE_VIOLATION", what, reason, "high", sig, inputs, replay, lo,
                end["line"] + 1, extra)
        elif cls == 108:
            if 0x700 <= dcode <= 0x71F or 0x780 <= dcode <= 0x79F:
                add("RESOURCE_LEAK", "fd",
                    "fd state differs from the baseline after pass %d (fd %d)"
                    % (1 if dcode < 0x780 else 2, dcode & 0x1f), "high",
                    ("fd", 1 if dcode < 0x780 else 2), inputs, replay, lo,
                    end["line"] + 1, extra)
            elif 0x740 <= dcode <= 0x75F:
                add("INVARIANT_FAILURE", "fd-probe",
                    "F_GETFL and fstat disagree on whether fd %d is open" % (dcode & 0x1f),
                    "high", ("fd-probe",), inputs, replay, lo, end["line"] + 1, extra)
            else:
                add("HARNESS_FAILURE", "precondition",
                    "baseline precondition not met (diag 0x%x)" % dcode, "high",
                    ("precondition", dcode), inputs, replay, lo, end["line"] + 1, extra)
        else:
            add("HARNESS_FAILURE", what, "executor self-check %s (diag 0x%x)" % (what, dcode),
                "high", (what,), inputs, replay, lo, end["line"] + 1, extra)

    # Crashes/hangs of the kernel, attributed to the open testcase if any.
    def crash_inputs():
        n = st.open_tc
        if n is None or n not in st.tcb:
            return None, None
        b = st.tcb[n]
        if b["src"] == "ir":
            return (dict(campaign=b["campaign"], testcase=n, src="ir", candidate=b["a"],
                         run=b["b"]), _replay_ir(b["a"]))
        inp = dict(campaign=b["campaign"], testcase=n, src=b["src"],
                   mode=b["a"], base=b["b"], mut=b["c"])
        rp = _replay_cov(b["a"], b["b"], b["c"]) if b["src"] == "cov" else None
        return inp, rp

    if st.kexc:
        e = st.kexc
        inp, rp = crash_inputs()
        frames = e["frames"][:3]
        sig = (e["vector"], e["err"], _cr2_class(e["cr2"])) + tuple(syms.func(a) for a in frames)
        extra = dict(fault=dict(vector=e["vector"], err=e["err"], rip=e["rip"], cr2=e["cr2"],
                                thread=e["thread"]),
                     backtrace=[syms.name(a) for a in e["frames"]])
        if inp is None:
            inp = dict(campaign=e.get("campaign"))
            rp = "none: not inside a UFUZZ testcase; rerun campaign %s" % e.get("campaign")
        add("KERNEL_EXCEPTION", "vector-%s" % e["vector"],
            "kernel took CPU exception %s (%s) at %s, cr2 %s" % (
                e["vector"], e.get("name", ""), syms.name(e["rip"] or 0),
                _cr2_class(e["cr2"])), "high" if st.open_tc is not None else "medium",
            sig, inp, rp, e["line"] - 3, e["line"] + 25, extra)
    if st.panic:
        p = st.panic
        inp, rp = crash_inputs()
        stem = re.sub(r"0x[0-9a-fA-F]+|\d+", "#", p["message"])
        sig = (stem,) + tuple(syms.func(a) for a in p["frames"][:3])
        extra = dict(panic_message=p["message"], backtrace=[syms.name(a) for a in p["frames"]])
        m = re.search(r"KFUZZ watchdog: target '(\w+)' hung on seed (0x[0-9a-f]+)", p["message"])
        if inp is None:
            inp = dict(campaign=p.get("campaign"))
            if m:
                rp = "in-kernel shell: fuzz replay %d %s" % (int(m.group(2), 16), m.group(1))
                inp.update(source="kfuzz", target=m.group(1), seed=m.group(2))
            else:
                rp = "none: not inside a UFUZZ testcase; rerun campaign %s" % p.get("campaign")
        add("KERNEL_PANIC", "kfuzz-watchdog" if m else "panic",
            "kernel panic: %s" % p["message"], "high" if (st.open_tc is not None or m) else "medium",
            sig, inp, rp, p["line"] - 3, p["line"] + 15, extra)
    if st.exit == "TIMEOUT" and not st.kexc and not st.panic:
        inp, rp = crash_inputs()
        if inp is None:
            inp = dict(campaign=st.last_campaign)
            rp = "none: hang outside a UFUZZ testcase; rerun campaign %s" % st.last_campaign
        add("KERNEL_HANG", "no-poweroff",
            "QEMU did not power off before the runner timeout", "medium",
            (inp.get("campaign"), inp.get("src"), inp.get("mode")), inp, rp,
            len(st.lines) - 12, len(st.lines))

    for campaign, d, line in st.drift:
        add("RESOURCE_LEAK", "memory",
            "batch not net-zero: %s pages, %s heap bytes" % (d.get("dpmm"), d.get("dheap")),
            "medium", (campaign,), dict(campaign=campaign),
            "none: batch-level; rerun campaign %s" % campaign, line - 3, line + 3,
            dict(resource_delta=dict(pmm_pages=ival(d, "dpmm"), heap_bytes=ival(d, "dheap"))))

    # KTEST assertion failures not already explained by a finding of their campaign.
    for campaign, where, expr, line in st.ktest_fail:
        if campaign in explained_campaigns:
            continue
        add("INVARIANT_FAILURE", "ktest", "KTEST assertion failed: %s (%s)" % (expr, where),
            "medium", (campaign, where), dict(campaign=campaign),
            "make test-suite SUITE=%s" % campaign.split(".")[0] if campaign else "none",
            line - 3, line + 2)

    if st.exit is None and not st.kexc and not st.panic:
        lo = st.infra[0][0] if st.infra else len(st.lines) - 10
        add("INFRA_FAILURE", "no-run", "no QEMU exit status in the log (build or launch failed)",
            "high", ("no-exit",), dict(campaign=st.last_campaign), "none: rerun the build",
            lo - 3, lo + 5)
    return findings, expected, counts


# ---- dedup + artifacts -------------------------------------------------------------
def sig_hash(sigkey):
    return hashlib.sha1(json.dumps(sigkey, default=str).encode()).hexdigest()[:16]


def dedup(findings):
    uniq = {}
    for f in findings:
        h = sig_hash(f["sigkey"])
        if h in uniq:
            u = uniq[h]
            u["occurrences"] += 1
            u.setdefault("other_inputs", []).append(f["inputs"])
        else:
            f = dict(f)
            f["signature"] = h
            uniq[h] = f
    return uniq


def missing_fields(f):
    return [k for k in REQUIRED if f.get(k) in (None, "", [], {}, "unknown", "unavailable")]


def write_artifacts(uniq, out):
    os.makedirs(out, exist_ok=True)
    for h, f in uniq.items():
        d = os.path.join(out, h)                      # hash only: never log text
        os.makedirs(d, exist_ok=True)
        body = {k: v for k, v in f.items() if k not in ("program_ir", "log_excerpt")}
        body["sigkey"] = [str(x) for x in f["sigkey"]]
        with open(os.path.join(d, "finding.json"), "w") as fh:
            json.dump(body, fh, indent=2, sort_keys=True, default=str)
        with open(os.path.join(d, "log.txt"), "w") as fh:
            fh.write(f["log_excerpt"] + "\n")
        if f.get("program_ir"):
            with open(os.path.join(d, "program.ir"), "w") as fh:
                fh.write(f["program_ir"])


def triage(paths, kernel=None, u4gen=None):
    all_f, all_exp = [], []
    counts = {"testcases": 0, "clean": 0, "expected_fail": 0, "expected_errno": 0}
    for p in paths:
        st = parse_log(p)
        syms = Symbols(kernel, st.build_id)
        f, e, c = classify_log(st, syms, u4gen)
        all_f += f
        all_exp += e
        for k in counts:
            counts[k] += c[k]
    return dedup(all_f), all_exp, counts, len(all_f)


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opts = {}
    it = iter(argv)
    for a in it:
        if a in ("--out", "--kernel", "--u4gen"):
            opts[a] = next(it)
    paths = [a for a in args if a not in opts.values()]
    if not paths:
        print(__doc__ if __doc__ else "usage: ufuzz_triage.py LOG...", file=sys.stderr)
        return 2
    out = opts.get("--out", os.path.join(ROOT, "build", "ufuzz-artifacts"))
    kernel = opts.get("--kernel", os.path.join(ROOT, "makhos.kernel"))
    u4gen = opts.get("--u4gen", os.path.join(ROOT, "build", "u4gen"))
    uniq, expected, counts, nraw = triage(paths, kernel, u4gen)
    write_artifacts(uniq, out)
    if "--quiet" not in argv:
        print("UFUZZ TRIAGE: %d log(s); %d testcases (%d clean, %d failed as designed); "
              "%d expected errno result(s) in clean programs"
              % (len(paths), counts["testcases"], counts["clean"], counts["expected_fail"],
                 counts["expected_errno"]))
        print("  %d finding(s), %d unique signature(s) -> %s"
              % (nraw, len(uniq), os.path.relpath(out, ROOT) if out.startswith(ROOT) else out))
        for h, f in sorted(uniq.items(), key=lambda kv: kv[1]["classification"]):
            print("  [%s] %-17s %-14s x%-3d %s" % (h, f["classification"], f["subkind"],
                                                   f["occurrences"], f["reason"][:90]))
            print("        replay: %s" % f["replay"])
    rc = 0
    if "--check" in argv:
        bad = {h: missing_fields(f) for h, f in uniq.items() if missing_fields(f)}
        for h, miss in bad.items():
            print("UFUZZ TRIAGE --check: %s incomplete, missing %s" % (h, ", ".join(miss)),
                  file=sys.stderr)
        rc |= 1 if bad else 0
    if "--fail-on-findings" in argv and uniq:
        print("UFUZZ TRIAGE: %d unique finding(s) in the run" % len(uniq), file=sys.stderr)
        rc |= 1
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
