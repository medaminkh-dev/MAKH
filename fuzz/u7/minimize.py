#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U7 — reproduction and IR minimization.
#
#   minimize.py ARTIFACT_DIR [--tree ROOT]
#
# ARTIFACT_DIR is a U6 artifact (finding.json) of a UFUZZ `cov` testcase. ROOT
# is the kernel tree to test against (default: this repository); its build is
# used for every boot. Steps (spec §4 U7):
#   1. replay the seed (`make test-suite SUITE=ufuzzreplay REPLAY=m:b:c`);
#   2. replay the serialized program through `run-ir` (original.ir: the C
#      generator's expectations);
#   3. delete calls (ddmin; deleting a call also deletes every call that uses a
#      handle it creates);
#   4. fewer processes/threads: NOT_APPLICABLE (generated programs are single-
#      process);
#   5./6. shrink lengths and buffers, simplify values;
#   7. every candidate is re-run; it is accepted only with the SAME U6
#      classification and signature at the reproduction threshold.
# Candidates must pass the U3 validator and are re-annotated by the U5.3
# deriver (the C expectations do not survive an edit); a candidate the deriver
# cannot fully predict is dropped. Candidate order is fixed, so the same results
# give the same minimal program.
#
# Thresholds: 3/3 for deterministic classes; TIMEOUT / KERNEL_HANG >= 2/5 with
# the measured rate recorded. Kernel crashes end a boot: the crashing candidate
# is recorded and the remaining ones run in the next boot; intermediate steps
# accept one crash, the final program must crash in 3/3 boots.
#
# Output (in ARTIFACT_DIR): original.ir, minimized.ir, minimize.json.

import copy
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "fuzz", "u3"))
sys.path.insert(0, os.path.join(ROOT, "fuzz", "u5"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import u3  # noqa: E402
import derive  # noqa: E402
import ufuzz_triage as T  # noqa: E402
import u7_batch  # noqa: E402

TIMING = ("TIMEOUT", "KERNEL_HANG")
CRASH = ("KERNEL_EXCEPTION", "KERNEL_PANIC")
CLEAN = "clean"


def threshold(cls):
    """(runs per candidate, runs that must reproduce)."""
    if cls in TIMING:
        return 5, 2
    if cls in CRASH:
        return 1, 1                    # one boot per crash; final check: 3/3 boots
    return 3, 3


# ---- candidates ----------------------------------------------------------------
_ABI = None


def abi():
    global _ABI
    if _ABI is None:
        _ABI = u3.Abi()
    return _ABI


def annotate(prog):
    """Replace every expectation with the deriver's prediction. Returns the
    program, or None if some call cannot be predicted."""
    p = copy.deepcopy(prog)
    m = derive.Model(p, abi())
    for c in p.calls:
        m.assumed = False
        try:
            pred = m.predict(c)
        except derive.Unknown:
            return None
        if m.assumed and pred[0] == "slot":
            pred = ("nonneg",)
        c.expect = tuple(pred)
    return p


def valid(prog):
    try:
        u3.validate(prog, abi())
        return True
    except u3.ValErr:
        return False


def delete_calls(prog, drop):
    """Delete the calls in `drop` and, transitively, every call that uses a
    handle created by a deleted call."""
    drop = set(drop)
    dead = set()
    changed = True
    while changed:
        changed = False
        for i, c in enumerate(prog.calls):
            if i in drop:
                if c.bind:
                    new = set(c.bind[1]) - dead
                    if new:
                        dead |= new
                        changed = True
                continue
            if any(op[0] == u3.RES and op[1] in dead for op in c.args):
                drop.add(i)
                changed = True
    p = copy.deepcopy(prog)
    p.calls = [c for i, c in enumerate(p.calls) if i not in drop]
    return p


def finish(prog):
    """A runnable candidate (valid, fully annotated) or None."""
    if not prog.calls or not valid(prog):
        return None
    return annotate(prog)


def value_edits(prog):
    """Single-value simplifications, in a fixed order."""
    out = []
    for i, c in enumerate(prog.calls):
        if c.sys in ("read", "write") and len(c.args) == 3 and c.args[2][0] == u3.IMM \
                and c.args[2][1] > 1:
            out.append(("len1", i, 2, (u3.IMM, 1)))
        if c.sys == "lseek" and c.args[1][0] == u3.IMM and c.args[1][1] != 0:
            out.append(("off0", i, 1, (u3.IMM, 0)))
        if c.sys == "fcntl" and len(c.args) == 3 and c.args[1] == (u3.IMM, 4) \
                and c.args[2] != (u3.IMM, 0):
            out.append(("setfl0", i, 2, (u3.IMM, 0)))
    return out


def apply_edit(prog, edit):
    _, i, ai, op = edit
    p = copy.deepcopy(prog)
    args = list(p.calls[i].args)
    args[ai] = op
    p.calls[i].args = args
    return p


def shrink_buffers(prog):
    """Cut each buffer to the largest length any call uses it with (>= 8)."""
    need = {}
    for c in prog.calls:
        for ai, op in enumerate(c.args):
            if op[0] == u3.BUF:
                n = 8
                if c.sys in ("read", "write") and c.args[2][0] == u3.IMM:
                    n = max(n, c.args[2][1])
                need[op[1]] = max(need.get(op[1], 0), n)
    p = copy.deepcopy(prog)
    p.buffers = [(bid, min(size, need.get(bid, 8)), data[:min(size, need.get(bid, 8))])
                 for bid, size, data in p.buffers]
    return p


def text(prog):
    return u3.serialize(prog)


# ---- execution -------------------------------------------------------------------
class QemuRunner:
    """Boots the ufuzzmin suite with a batch image; kernel crashes end a boot, so
    untested candidates go to the next boot."""

    def __init__(self, tree, workdir):
        self.tree, self.work = tree, workdir
        # Kernel-crash signatures name functions when the run's kernel ELF is
        # available (U6 checks its build id); use the tested tree's kernel so
        # signatures here match the artifact's.
        self.kernel = os.path.join(tree, "makhos.kernel")
        self.boots = 0
        os.makedirs(workdir, exist_ok=True)

    def _boot(self, programs, runs):
        n = self.boots
        self.boots += 1
        batch = os.path.join(self.work, "batch_%03d.ir" % n)
        img = os.path.join(self.work, "batch_%03d.img" % n)
        log = os.path.join(self.work, "boot_%03d.log" % n)
        with open(batch, "w") as f:
            f.write(u7_batch.render(programs))
        u7_batch.make_image(batch, img, os.path.join(self.tree, "build", "ext2.img"))
        timeout = 60 + 4 * len(programs) * runs
        rel = os.path.relpath(img, self.tree)
        with open(log, "w") as f:
            subprocess.run(["make", "-s", "CC=gcc-13", "test-suite", "SUITE=ufuzzmin",
                            "BATCH=%d" % runs, "EXT2=%s" % rel, "TIMEOUT=%d" % timeout],
                           cwd=self.tree, stdout=f, stderr=subprocess.STDOUT)
        return parse_boot(log, runs, len(programs), self.kernel)

    def run(self, programs, runs):
        return execute(self._boot, programs, runs)

    def replay_seed(self, mode, base, mut):
        log = os.path.join(self.work, "seed_replay.log")
        self.boots += 1
        with open(log, "w") as f:
            subprocess.run(["make", "-s", "CC=gcc-13", "test-suite", "SUITE=ufuzzreplay",
                            "REPLAY=%d:%d:%d" % (mode, base, mut), "TIMEOUT=600"],
                           cwd=self.tree, stdout=f, stderr=subprocess.STDOUT)
        st = T.parse_log(log)
        f, _, _ = T.classify_log(st, T.Symbols(self.kernel, st.build_id), None)
        sigs = [T.sig_hash(x["sigkey"]) for x in f]
        runs = len([n for n in st.tcb if st.tcb[n]["src"] == "cov"])
        return sigs, runs


def parse_boot(log, runs, count, kernel=None):
    """Per candidate: a list of per-run results (CLEAN or a signature hash).
    A candidate whose runs did not all happen (the boot died) is incomplete."""
    st = T.parse_log(log)
    findings, _, _ = T.classify_log(st, T.Symbols(kernel, st.build_id), None)
    by_tc = {}
    for f in findings:
        n = f["inputs"].get("testcase")
        if n is not None:
            by_tc[n] = T.sig_hash(f["sigkey"])
    res = [[] for _ in range(count)]
    for n, b in st.tcb.items():
        if b["src"] != "ir" or b["a"] is None or not (0 <= b["a"] < count):
            continue
        if n in st.tc:
            res[b["a"]].append(by_tc.get(n, CLEAN) if st.tc[n]["status"] != 0 else CLEAN)
        elif n in by_tc:                      # the boot died inside this run
            res[b["a"]].append(by_tc[n])
    return res


def execute(boot, programs, runs, max_boots=64):
    """Run every program `runs` times, rebooting after a crash for the programs
    that did not finish. Returns per-program result lists."""
    results = [[] for _ in programs]
    todo = list(range(len(programs)))
    for _ in range(max_boots):
        if not todo:
            break
        part = boot([programs[i] for i in todo], runs)
        nxt = []
        for j, i in enumerate(todo):
            results[i] += part[j]
            if len(part[j]) < runs and not any(r != CLEAN for r in part[j]):
                nxt.append(i)                 # never reached: next boot
        if nxt == todo:
            break                             # no progress: give up on these
        todo = nxt
    return results


def reproduces(results, sig, cls):
    runs, need = threshold(cls)
    return sum(1 for r in results if r == sig) >= need


# ---- the minimizer ----------------------------------------------------------------
def minimize(original, sig, cls, runner, log=None):
    """original: u3.Program with the generator's expectations. Returns
    (status, minimized program or None, stats)."""
    say = log or (lambda *_: None)
    runs, need = threshold(cls)
    stats = dict(candidates=0, rounds=0, steps=[])

    def test(cands):
        cands = [c for c in cands if c is not None]
        uniq, seen = [], set()
        for c in cands:
            t = text(c)
            if t not in seen:
                seen.add(t)
                uniq.append(c)
        if not uniq:
            return []
        stats["candidates"] += len(uniq)
        stats["rounds"] += 1
        res = runner.run([text(c) for c in uniq], runs)
        return [(c, r) for c, r in zip(uniq, res)]

    # Step 2: the serialized program itself.
    r0 = runner.run([text(original)], runs)[0]
    stats["original"] = "%d/%d" % (sum(1 for x in r0 if x == sig), len(r0))
    if not reproduces(r0, sig, cls):
        return "not-reproduced", None, stats

    cur = annotate(original) or original
    # The deriver's annotation must still reproduce before edits build on it.
    if text(cur) != text(original):
        r1 = runner.run([text(cur)], runs)[0]
        if not reproduces(r1, sig, cls):
            cur = original
            stats["steps"].append("deriver annotations do not reproduce; kept generator's")

    # Step 3: ddmin over calls.
    n = 2
    while len(cur.calls) >= 2:
        size = len(cur.calls)
        chunks = [list(range(size))[k * size // n:(k + 1) * size // n] for k in range(n)]
        cands = [finish(delete_calls(cur, ch)) for ch in chunks if ch]
        hit = None
        for c, r in test(cands):
            if reproduces(r, sig, cls) and (hit is None or len(c.calls) < len(hit.calls)):
                hit = c
        if hit is not None:
            stats["steps"].append("calls %d -> %d" % (len(cur.calls), len(hit.calls)))
            say("  calls %d -> %d" % (len(cur.calls), len(hit.calls)))
            cur, n = hit, max(n - 1, 2)
        elif n < size:
            n = min(2 * n, size)
        else:
            break

    # Steps 5/6: values and buffers, one accepted edit at a time.
    for _ in range(32):
        edits = value_edits(cur)
        cands = [finish(apply_edit(cur, e)) for e in edits]
        cands.append(finish(shrink_buffers(cur)) if text(shrink_buffers(cur)) != text(cur) else None)
        hit = None
        for c, r in test(cands):
            if reproduces(r, sig, cls):
                hit = c
                break
        if hit is None:
            break
        stats["steps"].append("value/buffer simplification")
        cur = hit

    # Final confirmation at the full threshold (crashes: 3 boots).
    if cls in CRASH:
        fin = []
        for _ in range(3):
            fin += runner.run([text(cur)], 1)[0]
        ok = sum(1 for x in fin if x == sig) >= 3
    else:
        fin = runner.run([text(cur)], runs)[0]
        ok = reproduces(fin, sig, cls)
    stats["minimized"] = "%d/%d" % (sum(1 for x in fin if x == sig), len(fin))
    if not ok:
        return "unstable", None, stats
    return "minimized", cur, stats


def main(argv):
    if not argv or argv[0].startswith("-"):
        print("usage: minimize.py ARTIFACT_DIR [--tree ROOT]", file=sys.stderr)
        return 2
    art = argv[0]
    tree = argv[argv.index("--tree") + 1] if "--tree" in argv else ROOT
    with open(os.path.join(art, "finding.json")) as f:
        fnd = json.load(f)
    inp = fnd.get("inputs", {})
    if inp.get("src") != "cov":
        print("not applicable: only UFUZZ cov testcases can be minimized", file=sys.stderr)
        return 2
    sig, cls = fnd["signature"], fnd["classification"]
    mode, base, mut = inp["mode"], inp["base"], inp["mut"]
    runner = QemuRunner(tree, os.path.join(art, "u7work"))
    report = dict(signature=sig, classification=cls, threshold="%d/%d" % threshold(cls)[::-1],
                  inputs=dict(mode=mode, base=base, mut=mut), tree=os.path.abspath(tree))

    sigs, nrun = runner.replay_seed(mode, base, mut)
    report["seed_replay"] = "%d/%d" % (sigs.count(sig), nrun)
    print("seed replay: %s" % report["seed_replay"])

    ir = u7_batch.gen_ir(mode, base, mut, os.path.join(tree, "build", "u4gen"))
    with open(os.path.join(art, "original.ir"), "w") as f:
        f.write(ir)
    original = u3.deserialize(ir)
    status, mini, stats = minimize(original, sig, cls, runner, log=print)
    report.update(stats)
    report.update(status=status, calls_before=len(original.calls), boots=runner.boots,
                  steps_note="step 4 (processes/threads): NOT_APPLICABLE")
    if mini is not None:
        report["calls_after"] = len(mini.calls)
        with open(os.path.join(art, "minimized.ir"), "w") as f:
            f.write(u7_batch.render([text(mini)]))
        report["replay"] = ("python3 tools/u7_batch.py image %s build/u7.img && make test-suite "
                            "SUITE=ufuzzmin BATCH=3 EXT2=build/u7.img"
                            % os.path.join(art, "minimized.ir"))
    with open(os.path.join(art, "minimize.json"), "w") as f:
        json.dump(report, f, indent=2, sort_keys=True)
    print("%s: calls %d -> %s, boots %d, original %s, minimized %s"
          % (status, len(original.calls), report.get("calls_after", "-"), runner.boots,
             stats.get("original"), stats.get("minimized", "-")))
    return 0 if status == "minimized" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
