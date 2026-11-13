#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U8 — host-driven generational campaign.
#
#   campaign.py ARM META_SEED --gens G --batch B [--runs 2] --tree ROOT --out DIR
#       ARM is `guided` (corpus + structure-aware mutation) or `random` (fresh
#       generator programs). Generation 0 is the SAME B fresh programs for both
#       arms of a meta-seed, so a pair starts from one point.
#   campaign.py compare DIR...
#       apply the pre-registered rule to finished campaigns (see RULE)
#
# One generation = one boot (more only if a kernel crash ends a boot early):
# the batch and the host's global virgin map go into a copy of the ext2 image;
# `make test-suite SUITE=ufuzzcamp CAMP=<runs>` runs every program <runs> times
# with kernel coverage on and prints each run's edges that are new against that
# map (U8 `UFZ1 cov` / `UFZ1 covid`).
#
# Noise filter: an edge counts only if EVERY run of the program hit it (with
# runs=2: both). Edges seen in some runs only are counted as noise and reported.
# Keep (guided corpus): a clean program with a confirmed globally-new edge, or a
# new resource-state feature (syscall, outcome) from the deriver's model.
# Findings (any non-clean run, classified by U6 triage) go to the crash corpus.

import json
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import corpus as C  # noqa: E402
import mutate as X  # noqa: E402

M = X.M
u3, T, B7 = M.u3, M.T, M.u7_batch
CLEAN = M.CLEAN

RULE = ("guided > random iff guided has more confirmed kernel edges in >= 4 of 5 pairs "
        "AND mean(guided - random) > max(range of guided finals, range of random finals)")


# ---- the guest boot ----------------------------------------------------------------
def parse_camp_log(log, count, kernel=None):
    """Per program: a list of runs, each {"result": CLEAN|signature, "edges": set,
    "truncated": bool}. Runs that did not happen (boot ended) are absent."""
    st = T.parse_log(log)
    findings, _, _ = T.classify_log(st, T.Symbols(kernel, st.build_id), None)
    sig_of = {}
    for f in findings:
        n = f["inputs"].get("testcase")
        if n is not None:
            sig_of[n] = (T.sig_hash(f["sigkey"]), f["classification"])
    edges, total = {}, {}
    for line in st.lines:
        m = T.REC.match(line)
        if not m:
            continue
        kind, d = m.group(1), T.kv(m.group(2))
        if kind == "cov":
            total[T.ival(d, "n")] = T.ival(d, "new", 0)
            edges.setdefault(T.ival(d, "n"), set())
        elif kind == "covid":
            n = T.ival(d, "n")
            edges.setdefault(n, set()).update(int(x, 16) for x in d.get("ids", "").split(",") if x)
    out = [[] for _ in range(count)]
    for n in sorted(k for k in st.tcb if k is not None):
        b = st.tcb[n]
        if b["src"] != "ir" or b["a"] is None or not (0 <= b["a"] < count):
            continue
        if n in st.tc:
            res = sig_of.get(n, (CLEAN, None)) if st.tc[n]["status"] != 0 else (CLEAN, None)
        elif n in sig_of:
            res = sig_of[n]                       # the boot died in this run
        else:
            continue
        e = edges.get(n, set())
        out[b["a"]].append({"result": res[0], "classification": res[1], "edges": e,
                            "truncated": total.get(n, 0) > len(e)})
    return out


class QemuRunner:
    def __init__(self, tree, work):
        self.tree, self.work, self.boots = tree, work, 0
        self.kernel = os.path.join(tree, "makhos.kernel")
        os.makedirs(work, exist_ok=True)

    def boot(self, programs, runs, virgin):
        n = self.boots
        self.boots += 1
        batch = os.path.join(self.work, "gen_%03d.ir" % n)
        vbin = os.path.join(self.work, "virgin_%03d.bin" % n)
        img = os.path.join(self.work, "gen_%03d.img" % n)
        log = os.path.join(self.work, "boot_%03d.log" % n)
        with open(batch, "w") as f:
            f.write(B7.render(programs))
        bits = bytearray(8192)
        for e in virgin:
            bits[e >> 3] |= 1 << (e & 7)              # little-endian 64-bit words
        with open(vbin, "wb") as f:
            f.write(bytes(bits))
        B7.make_image(batch, img, os.path.join(self.tree, "build", "ext2.img"), virgin=vbin)
        timeout = 90 + 3 * len(programs) * runs
        with open(log, "w") as f:
            subprocess.run(["make", "-s", "CC=gcc-13", "test-suite", "SUITE=ufuzzcamp",
                            "CAMP=%d" % runs, "EXT2=%s" % os.path.relpath(img, self.tree),
                            "TIMEOUT=%d" % timeout],
                           cwd=self.tree, stdout=f, stderr=subprocess.STDOUT)
        for p in (img, vbin):
            os.remove(p)                              # 2 MiB per boot otherwise
        return parse_camp_log(log, len(programs), self.kernel)

    def run(self, programs, runs, virgin):
        """Every program `runs` times; a kernel crash ends a boot and the
        programs that did not run go to the next boot."""
        results = [[] for _ in programs]
        todo = list(range(len(programs)))
        for _ in range(16):
            if not todo:
                break
            part = self.boot([programs[i] for i in todo], runs, virgin)
            nxt = []
            for j, i in enumerate(todo):
                results[i] += part[j]
                if len(part[j]) < runs and all(r["result"] == CLEAN for r in part[j]):
                    nxt.append(i)
            if nxt == todo:
                break
            todo = nxt
        return results


# ---- state features (resource-state transitions) -------------------------------------
def features(prog):
    """(syscall, outcome class) pairs the deriver predicts for a program — for a
    clean program, what the kernel actually returned."""
    m = M.derive.Model(prog, M.abi())
    out = set()
    for c in prog.calls:
        try:
            p = m.predict(c)
        except M.derive.Unknown:
            break
        if p[0] == "exact" and p[1] < 0:
            out.add("%s:%s" % (c.sys, M.derive.ERRNAME.get(-p[1], p[1])))
        elif p[0] == "exact":
            out.add("%s:%s" % (c.sys, "zero" if p[1] == 0 else "value"))
        else:
            out.add("%s:%s" % (c.sys, p[0]))
    return out


# ---- one campaign ---------------------------------------------------------------------
class Campaign:
    def __init__(self, arm, meta_seed, batch, runs, runner, u4gen):
        assert arm in ("guided", "random")
        self.arm, self.seed, self.B, self.runs = arm, meta_seed, batch, runs
        self.runner, self.u4gen = runner, u4gen
        self.rng = random.Random(meta_seed * 2 + (arm == "guided"))
        self.corpus, self.crashes = C.Corpus(), C.CrashCorpus()
        self.confirmed, self.feats, self.sysreach = set(), set(), set()
        self.metrics, self.execs, self.first_new = [], 0, None

    def fresh(self, rng, n):
        out = []
        for _ in range(n):
            mode = (0, 5)[rng.randrange(2)]
            b, m = rng.randrange(1 << 20), rng.randrange(1 << 20)
            out.append((u3.deserialize(B7.gen_ir(mode, b, m, self.u4gen)),
                        {"origin": "generator", "mode": mode, "base": b, "mut": m}))
        return out

    def pick(self, g):
        ids = self.corpus.order
        w = [1 + min(self.corpus.entries[i].new_edges, 50) +
             (10 if self.corpus.entries[i].generation >= g - 2 else 0) for i in ids]
        x = self.rng.uniform(0, sum(w))
        for i, wi in zip(ids, w):
            x -= wi
            if x <= 0:
                return self.corpus.entries[i]
        return self.corpus.entries[ids[-1]]

    def mutants(self, g):
        out, tries = [], 0
        while len(out) < self.B and tries < 25 * self.B:
            tries += 1
            parent = self.pick(g)
            donor = self.fresh(self.rng, 1)[0][0]
            other = u3.deserialize(self.pick(g).ir)
            prog, ops = X.mutant(u3.deserialize(parent.ir), self.rng, donor, other)
            if prog is not None:
                out.append((prog, {"origin": "mutation", "parents": [parent.id], "ops": ops}))
        return out, (len(out) / tries if tries else 0.0)

    def generation(self, g, programs, valid_ratio):
        t0 = time.time()
        texts = [u3.serialize(p) for p, _ in programs]
        res = self.runner.run(texts, self.runs, self.confirmed)
        new_gen = noise = 0
        for (prog, prov), text, runs in zip(programs, texts, res):
            self.execs += len(runs)
            if not runs:
                continue
            bad = [r for r in runs if r["result"] != CLEAN]
            if bad:
                e = C.Entry(text, dict(prov, reasons=["finding"]), generation=g)
                for r in bad:
                    self.crashes.add(r["result"], r["classification"], e)
                continue
            if len(runs) < self.runs:
                continue
            inter = set.intersection(*[r["edges"] for r in runs])
            union = set.union(*[r["edges"] for r in runs])
            noise += len(union - inter)
            novel = inter - self.confirmed
            self.confirmed |= novel
            new_gen += len(novel)
            if novel and self.first_new is None and g > 0:
                self.first_new = self.execs
            fs = features(prog) - self.feats
            self.feats |= fs
            self.sysreach |= {c.sys for c in prog.calls}
            reasons = (["seed"] if g == 0 else []) + (["new-edge"] if novel else []) + \
                      (["new-state"] if fs else [])
            if (novel or fs or g == 0):
                self.corpus.add(C.Entry(text, dict(prov, reasons=reasons or ["seed"]),
                                        new_edges=len(novel), features=fs, generation=g))
        self.metrics.append(dict(
            arm=self.arm, meta_seed=self.seed, gen=g, programs=len(programs), execs=self.execs,
            boots=self.runner.boots, confirmed_edges=len(self.confirmed), new_edges=new_gen,
            noise_edges=noise, corpus=len(self.corpus), crash_signatures=len(self.crashes.by_sig),
            valid_ratio=round(valid_ratio, 3), syscalls_reached=len(self.sysreach),
            state_features=len(self.feats), first_new_after_gen0=self.first_new,
            seconds=round(time.time() - t0, 1)))
        return self.metrics[-1]

    def run(self, gens, log=None):
        gen0 = self.fresh(random.Random(self.seed), self.B)    # identical for both arms
        self.generation(0, gen0, 1.0)
        if log:
            log(self.metrics[-1])
        for g in range(1, gens + 1):
            if self.arm == "random":
                progs, ratio = self.fresh(self.rng, self.B), 1.0
            else:
                progs, ratio = self.mutants(g)
            self.generation(g, progs, ratio)
            if log:
                log(self.metrics[-1])

    def save(self, out):
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, "metrics.jsonl"), "w") as f:
            for row in self.metrics:
                f.write(json.dumps(row, sort_keys=True) + "\n")
        self.corpus.export(os.path.join(out, "corpus.jsonl"))
        self.crashes.export(os.path.join(out, "crashes.jsonl"))


# ---- the pre-registered comparison -----------------------------------------------------
def compare(rows):
    """rows: final metrics rows of both arms for every meta-seed. Returns
    (verdict, table, numbers)."""
    by = {}
    for r in rows:
        by.setdefault(r["meta_seed"], {})[r["arm"]] = r["confirmed_edges"]
    seeds = sorted(s for s in by if "guided" in by[s] and "random" in by[s])
    g = [by[s]["guided"] for s in seeds]
    r = [by[s]["random"] for s in seeds]
    wins = sum(1 for a, b in zip(g, r) if a > b)
    diff = [a - b for a, b in zip(g, r)]
    mean_diff = sum(diff) / len(diff) if diff else 0.0
    spread = max(max(g) - min(g), max(r) - min(r)) if seeds else 0
    claim = len(seeds) == 5 and wins >= 4 and mean_diff > spread
    table = [(s, by[s]["guided"], by[s]["random"], by[s]["guided"] - by[s]["random"]) for s in seeds]
    verdict = ("guided > random (pre-registered rule met)" if claim else
               "no measurable difference under the pre-registered rule")
    return verdict, table, dict(pairs=len(seeds), wins=wins, mean_diff=mean_diff, spread=spread)


def main(argv):
    if argv and argv[0] == "compare":
        rows = []
        for d in argv[1:]:
            with open(os.path.join(d, "metrics.jsonl")) as f:
                rows.append([json.loads(l) for l in f if l.strip()][-1])
        verdict, table, nums = compare(rows)
        print("rule: " + RULE)
        print("%-10s %8s %8s %6s" % ("meta-seed", "guided", "random", "diff"))
        for s, gg, rr, dd in table:
            print("%-10s %8d %8d %+6d" % (s, gg, rr, dd))
        print("wins %d/%d, mean diff %.1f, max within-arm spread %d -> %s"
              % (nums["wins"], nums["pairs"], nums["mean_diff"], nums["spread"], verdict))
        return 0
    if len(argv) < 2 or argv[0] not in ("guided", "random"):
        print("usage: campaign.py guided|random META_SEED --gens G --batch B [--runs 2] "
              "--tree ROOT --out DIR  |  campaign.py compare DIR...", file=sys.stderr)
        return 2
    arm, seed = argv[0], int(argv[1])
    opt = lambda k, d: argv[argv.index(k) + 1] if k in argv else d  # noqa: E731
    gens, batch, runs = int(opt("--gens", 10)), int(opt("--batch", 16)), int(opt("--runs", 2))
    tree, out = opt("--tree", ROOT), opt("--out", os.path.join(ROOT, "build", "u8"))
    runner = QemuRunner(tree, os.path.join(out, "boots"))
    camp = Campaign(arm, seed, batch, runs, runner, os.path.join(tree, "build", "u4gen"))
    camp.run(gens, log=lambda r: (print(json.dumps(r, sort_keys=True)), sys.stdout.flush()))
    camp.save(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
