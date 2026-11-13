#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U7 — minimizer selftest (host only, no QEMU).
#
# The minimizer is driven by a MOCK runner: a simulated kernel bug decides,
# from a candidate program's calls, whether a run reproduces the target
# signature. This tests the minimizer's own logic — deletion closure,
# validity, re-annotation, determinism, signature-drift rejection, thresholds,
# crash/reboot handling — independently of any real kernel. End-to-end runs on
# real mutated kernels are documented in docs/UFUZZ_U7_MINIMIZE.md.

import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import minimize as M  # noqa: E402

u3 = M.u3
FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def gen(mode, base, mut):
    return u3.deserialize(M.u7_batch.gen_ir(mode, base, mut))


def getfl_on_socket(prog):
    """Simulated bug: F_GETFL on a socket's own handle misbehaves."""
    socks = set()
    for c in prog.calls:
        if c.sys == "socket" and c.bind and c.args[0] == (u3.IMM, 2):
            socks.add(c.bind[1][0])
        if c.sys == "fcntl" and c.args[1] == (u3.IMM, 3) and c.args[0][0] == u3.RES \
                and c.args[0][1] in socks:
            return True
    return False


class Mock:
    """Runs programs through `sim(prog, run) -> CLEAN | signature`."""

    def __init__(self, sim):
        self.sim, self.boots, self.batches = sim, 0, []

    def run(self, programs, runs):
        self.boots += 1
        self.batches.append(len(programs))
        out = []
        for t in programs:
            p = u3.deserialize(t)
            out.append([self.sim(p, r) for r in range(runs)])
        return out


def find_seed(pred, mode=5, limit=400):
    for s in range(limit):
        p = gen(mode, s, s * 7919 + 1)
        if pred(p):
            return s, p
    raise SystemExit("no seed found for the simulated bug")


def main():
    seed, prog = find_seed(getfl_on_socket)

    # (1) annotation: on unedited mode-5 programs the deriver reproduces the
    #     generator's expectations exactly (U5.3), so re-annotation is lossless.
    for s in range(24):
        p = gen(5, s, s * 7919 + 1)
        a = M.annotate(p)
        check(a is not None and u3.serialize(a) == u3.serialize(p),
              "annotate: seed %d re-annotation differs from the generator" % s)

    # (2) deletion closure: every candidate is valid, fully annotated, and no
    #     call uses a handle whose creator was deleted.
    rnd = random.Random(7)
    for _ in range(300):
        k = rnd.randint(1, max(1, len(prog.calls) // 2))
        drop = rnd.sample(range(len(prog.calls)), k)
        c = M.finish(M.delete_calls(prog, drop))
        if c is None:
            continue
        check(M.valid(c), "closure: invalid candidate after deleting %r" % sorted(drop))
        check(all(x.expect and x.expect[0] != "any" for x in c.calls),
              "closure: candidate not fully annotated")

    # (3) minimization of the simulated bug: small, contains the bug, deterministic.
    sim = lambda p, r: "SIG" if getfl_on_socket(p) else M.CLEAN  # noqa: E731
    st1, m1, s1 = M.minimize(prog, "SIG", "ORACLE_VIOLATION", Mock(sim))
    st2, m2, s2 = M.minimize(prog, "SIG", "ORACLE_VIOLATION", Mock(sim))
    check(st1 == "minimized" and m1 is not None and getfl_on_socket(m1),
          "minimize: status %r / bug lost" % st1)
    check(m1 is not None and len(m1.calls) <= 3,
          "minimize: %d calls left (expected socket + F_GETFL [+ close])"
          % (len(m1.calls) if m1 else -1))
    check(m1 is not None and m2 is not None and u3.serialize(m1) == u3.serialize(m2)
          and s1 == s2, "minimize: not deterministic")

    # (4) signature drift: any reduction fails with a DIFFERENT signature -> the
    #     original is kept, never a candidate with another signature.
    full = len(prog.calls)
    drift = lambda p, r: "SIG" if len(p.calls) == full else "OTHER"  # noqa: E731
    st, m, _ = M.minimize(prog, "SIG", "ORACLE_VIOLATION", Mock(drift))
    check(st == "minimized" and m is not None and len(m.calls) == full,
          "drift: a candidate with another signature was accepted (%s, %s calls)"
          % (st, len(m.calls) if m else None))

    # (5) a finding that does not reproduce: no minimized program.
    st, m, _ = M.minimize(prog, "SIG", "ORACLE_VIOLATION", Mock(lambda p, r: M.CLEAN))
    check(st == "not-reproduced" and m is None, "not-reproduced: got %r" % st)

    # (6) thresholds: deterministic classes need every run; timing classes >= 2/5.
    flaky2 = lambda p, r: "SIG" if (getfl_on_socket(p) and r in (1, 3)) else M.CLEAN  # noqa: E731
    flaky1 = lambda p, r: "SIG" if (getfl_on_socket(p) and r == 2) else M.CLEAN  # noqa: E731
    st, _, s = M.minimize(prog, "SIG", "TIMEOUT", Mock(flaky2))
    check(st == "minimized" and s.get("original") == "2/5", "timing 2/5: %r %r" % (st, s.get("original")))
    st, _, _ = M.minimize(prog, "SIG", "TIMEOUT", Mock(flaky1))
    check(st == "not-reproduced", "timing 1/5 must not reproduce: %r" % st)
    st, _, _ = M.minimize(prog, "SIG", "ORACLE_VIOLATION", Mock(flaky2))
    check(st == "not-reproduced", "deterministic class at 2/3 must not reproduce: %r" % st)

    # (7) crash handling: a boot ends at the first crashing program; the rest
    #     run in the next boot, and every program ends with a complete result.
    progs = [u3.serialize(gen(5, s, s)) for s in range(6)]
    crashers = {1, 4}

    def boot(batch, runs):
        out = []
        for t in batch:
            k = progs.index(t)
            if k in crashers:
                out.append(["CRASH"])
                out += [[] for _ in range(len(batch) - len(out))]
                return out
            out.append([M.CLEAN] * runs)
        return out
    res = M.execute(boot, progs, 1)
    check([r[0] if r else None for r in res] ==
          ["clean", "CRASH", "clean", "clean", "CRASH", "clean"],
          "crash: per-program results %r" % res)

    if FAILS:
        for f in FAILS:
            print("U7 MINIMIZER SELFTEST: FAIL — " + f, file=sys.stderr)
        sys.exit(1)
    print("U7 MINIMIZER SELFTEST: PASS — re-annotation lossless on 24 programs; 300 random "
          "deletions stay valid; simulated bug (seed %d, %d calls) -> %d calls, "
          "deterministic; drift, not-reproduced, threshold and crash cases hold."
          % (seed, len(prog.calls), len(m1.calls) if m1 else -1))


if __name__ == "__main__":
    main()
