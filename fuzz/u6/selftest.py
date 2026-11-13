#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U6 — triage selftest (host only, no QEMU).
#
# REAL fixtures (fuzz/u6/fixtures/*.log) are serial logs of actual runs,
# filtered to the lines tools/ufuzz_triage.py reads (filter_log below):
#   clean.log              ufuzz suite, clean tree: 0 findings, 9 canaries
#   oracle_getfl.log       mutation: F_GETFL returns 0       -> ORACLE_VIOLATION
#   kexc_dup2.log          mutation: dup2 takes no reference -> KERNEL_EXCEPTION #PF
#   kexc_dup2_replay.log   that finding's replay command, in a fresh boot
#   leak_pipe_efault.log   mutation: do_pipe keeps fds on -EFAULT -> RESOURCE_LEAK
#   kfuzz_watchdog.log     stress run: KFUZZ watchdog panic (predates build ids)
# SYNTHETIC cases are derived here from the real fixtures by a stated, minimal
# edit (one record changed or appended); they are never committed as logs.
#
# Every case asserts the exact classification; dedup cases assert signature
# counts; completeness and untrusted-input cases exercise --check and paths.

import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ufuzz_triage as T  # noqa: E402

FX = os.path.join(HERE, "fixtures")
KEEP = re.compile(r"^(UFZ1 |\[ (RUN  |PASS |FAIL )\]|\s+FAIL \S+:\d+:|qemu: |=== |"
                  r"!!! EXCEPTION|Exception: |Error code: |RIP=|Page fault address: |"
                  r"Thread: |\s+#[-0-9]+\s+0x|\*\*\* KERNEL PANIC|KFUZZ watchdog|Backtrace:|"
                  r"make: \*\*\*|  u3 self-check FAIL)")


def filter_log(src, dst):
    """How the real fixtures were produced from a full run log."""
    with open(src, errors="replace") as f, open(dst, "w") as o:
        o.writelines(l for l in f if KEEP.match(l))


FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def run(paths):
    uniq, expected, counts, nraw = T.triage(paths, kernel=None, u4gen=None)
    return uniq, counts, nraw


def classes(uniq):
    return sorted((f["classification"], f["subkind"]) for f in uniq.values())


def fx(name):
    return os.path.join(FX, name)


def lines(name):
    with open(fx(name)) as f:
        return f.read().splitlines()


def derive(tmp, name, src, edit):
    """Write a synthetic log: `edit` maps the real fixture's lines to new lines."""
    p = os.path.join(tmp, name)
    with open(p, "w") as f:
        f.write("\n".join(edit(lines(src))) + "\n")
    return p


def main():
    tmp = tempfile.mkdtemp(prefix="u6self")
    try:
        # ---- real fixtures ------------------------------------------------------
        u, c, n = run([fx("clean.log")])
        check(n == 0, "clean: expected 0 findings, got %d" % n)
        check(c["testcases"] == 488 and c["expected_fail"] == 9,
              "clean: testcase accounting %r" % c)

        u, c, n = run([fx("oracle_getfl.log")])
        check(n == 17 and len(u) == 1, "oracle: 17 findings / 1 signature, got %d/%d" % (n, len(u)))
        f = next(iter(u.values())) if u else {}
        check(f.get("classification") == "ORACLE_VIOLATION", "oracle: class %r" % f.get("classification"))
        check(re.fullmatch(r"make test-suite SUITE=ufuzzreplay REPLAY=5:\d+:\d+", f.get("replay", "")),
              "oracle: replay command %r" % f.get("replay"))
        check(not T.missing_fields(f), "oracle: incomplete %r" % T.missing_fields(f))

        u, c, n = run([fx("kexc_dup2.log")])
        f = next(iter(u.values())) if u else {}
        check(classes(u) == [("KERNEL_EXCEPTION", "vector-14")], "kexc: %r" % classes(u))
        check(f.get("inputs", {}).get("mode") == 5 and f.get("confidence") == "high",
              "kexc: not attributed to its open testcase: %r" % f.get("inputs"))
        check("cr2 null-page" in f.get("reason", ""), "kexc: cr2 class %r" % f.get("reason"))
        u2, _, n2 = run([fx("kexc_dup2.log"), fx("kexc_dup2_replay.log")])
        check(n2 == 2 and len(u2) == 1, "kexc+replay: same fault in two boots must be 1 "
              "signature x2, got %d/%d" % (n2, len(u2)))

        u, c, n = run([fx("leak_pipe_efault.log")])
        cl = classes(u)
        check(("RESOURCE_LEAK", "fd") in cl, "leak: no RESOURCE_LEAK fd in %r" % cl)
        check(len(u) == 6, "leak: expected 6 symptom signatures, got %d" % len(u))
        u3, _, _ = run([fx("leak_pipe_efault.log"), fx("oracle_getfl.log")])
        check(len(u3) == 7, "leak+oracle: different faults must not share a signature "
              "(expected 6 + 1, got %d)" % len(u3))

        u, c, n = run([fx("kfuzz_watchdog.log")])
        f = next(iter(u.values())) if u else {}
        check(classes(u) == [("KERNEL_PANIC", "kfuzz-watchdog")], "kfuzz: %r" % classes(u))
        check(re.fullmatch(r"in-kernel shell: fuzz replay \d+ ext2img", f.get("replay", "")),
              "kfuzz: replay %r" % f.get("replay"))
        check(T.missing_fields(f) == ["build_id"], "kfuzz: --check must flag the missing "
              "build id, got %r" % T.missing_fields(f))

        # ---- synthetic, each one stated edit of a real fixture --------------------
        # (a) hang: the clean run cut right after a testcase begins, then the
        #     runner's timeout markers (as tools/run_tests.py prints them).
        def cut_hang(L):
            i = max(k for k, l in enumerate(L) if l.startswith("UFZ1 tcb n=200 "))
            return L[:i + 1] + ["=== qemu exit status: TIMEOUT ===",
                                "=== RESULT: FAIL (timeout: kernel did not power off in time) ==="]
        u, _, _ = run([derive(tmp, "hang.log", "clean.log", cut_hang)])
        f = next(iter(u.values())) if u else {}
        check(classes(u) == [("KERNEL_HANG", "no-poweroff")] and
              f.get("inputs", {}).get("testcase") == 200, "hang: %r %r" % (classes(u), f.get("inputs")))

        # (b) a canary that passes: one `expect=fail` testcase ends with status 0.
        def canary_ok(L):
            out, done = [], False
            for l in L:
                if not done and l.startswith("UFZ1 tc ") and "expect=fail" in l:
                    l, done = re.sub(r"status=-?\d+", "status=0", l), True
                out.append(l)
            return out
        u, _, _ = run([derive(tmp, "canary.log", "clean.log", canary_ok)])
        check(classes(u) == [("HARNESS_FAILURE", "canary-passed")], "canary: %r" % classes(u))

        # (c) same RIP, different vector and error code: must NOT merge.
        def other_vector(L):
            return [l.replace("(vector 14)", "(vector 13)").replace("Error code: 0x0",
                                                                     "Error code: 0x10")
                    for l in L]
        u, _, _ = run([fx("kexc_dup2.log"), derive(tmp, "kexc13.log", "kexc_dup2.log", other_vector)])
        check(len(u) == 2, "vector: same RIP, different vector must be 2 signatures, got %d" % len(u))

        # (d) a worker fault: a clean cov testcase ends with status 2 (USER_FAULT)
        #     plus the controller and kernel records that accompany it.
        def user_fault(L):
            out, done = [], False
            for l in L:
                m = re.match(r"UFZ1 tc n=(\d+) cpid=(\d+) status=0 expect=ok", l)
                if not done and m and int(m.group(1)) > 40:
                    cp = int(m.group(2))
                    out += ["UFZ1 ufault pid=%d vec=14 err=0x6 rip=0x40123a cr2=0x0" % (cp + 1),
                            "UFZ1 res src=cov cpid=%d wpid=%d a=5 b=0 c=0 outcome=USER_FAULT status=139"
                            % (cp, cp + 1)]
                    l, done = l.replace("status=0", "status=2"), True
                out.append(l)
            return out
        u, _, _ = run([derive(tmp, "ufault.log", "clean.log", user_fault)])
        check(classes(u) == [("USER_FAULT", "vector-14")], "ufault: %r" % classes(u))

        # (e) a worker timeout (watchdog SIGKILL -> controller outcome 3).
        def timeout(L):
            out, done = [], False
            for l in L:
                m = re.match(r"UFZ1 tc n=(\d+) cpid=(\d+) status=0 expect=ok", l)
                if not done and m and int(m.group(1)) > 40:
                    out.append("UFZ1 res src=cov cpid=%s wpid=0 a=5 b=0 c=0 outcome=TIMEOUT "
                               "status=137" % m.group(2))
                    l, done = l.replace("status=0", "status=3"), True
                out.append(l)
            return out
        u, _, _ = run([derive(tmp, "timeout.log", "clean.log", timeout)])
        check(classes(u) == [("TIMEOUT", "watchdog")], "timeout: %r" % classes(u))

        # (f) batch drift.
        u, _, _ = run([derive(tmp, "drift.log", "clean.log",
                              lambda L: L[:300] + ["UFZ1 drift dpmm=1 dheap=64"] + L[300:])])
        check(classes(u) == [("RESOURCE_LEAK", "memory")], "drift: %r" % classes(u))

        # (g) no QEMU exit status: the build or launch failed.
        u, _, _ = run([derive(tmp, "infra.log", "clean.log",
                              lambda L: [l for l in L[:5] if not l.startswith("===")]
                              + ["make: *** [Makefile:1: x] Error 1"])])
        check(classes(u) == [("INFRA_FAILURE", "no-run")], "infra: %r" % classes(u))

        # (h) untrusted record values never reach a path: artifact dirs are hashes.
        def hostile(L):
            return L[:300] + ["UFZ1 drift dpmm=../../x dheap=$(touch_pwned)"] + L[300:]
        out = os.path.join(tmp, "art")
        u, _, _ = run([derive(tmp, "hostile.log", "clean.log", hostile)])
        T.write_artifacts(u, out)
        names = os.listdir(out)
        check(all(re.fullmatch(r"[0-9a-f]{16}", x) for x in names) and
              not os.path.exists(os.path.join(tmp, "x")), "hostile: unsafe artifact paths %r" % names)

        # ---- CLI gates ----------------------------------------------------------
        q = os.path.join(tmp, "cli")
        check(T.main([fx("clean.log"), "--out", q, "--quiet", "--fail-on-findings", "--check"]) == 0,
              "cli: clean run must pass --fail-on-findings --check")
        check(T.main([fx("oracle_getfl.log"), "--out", q, "--quiet", "--fail-on-findings"]) != 0,
              "cli: a finding must fail --fail-on-findings")
        check(T.main([fx("kfuzz_watchdog.log"), "--out", q, "--quiet", "--check"]) != 0,
              "cli: an incomplete artifact must fail --check")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if FAILS:
        for m in FAILS:
            print("U6 TRIAGE SELFTEST: FAIL — " + m, file=sys.stderr)
        sys.exit(1)
    print("U6 TRIAGE SELFTEST: PASS — 6 real fixtures and 8 derived cases classified "
          "exactly; dedup, completeness and path-safety checks hold.")


if __name__ == "__main__":
    main()
