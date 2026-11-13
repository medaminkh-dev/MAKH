#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U8 — corpus / mutation / campaign selftest (host only, no QEMU).
#
#   - fixtures/camp_sample.log: a REAL `ufuzzcamp` boot (6 programs x 2 runs,
#     empty virgin map), filtered to the lines the parser reads. It shows the
#     coverage noise the 2-run filter exists for.
#   - everything else runs the campaign against a MOCK runner (a simulated
#     kernel whose coverage is a function of the program), so the logic —
#     filter, keep rules, determinism, pairing, the pre-registered comparison —
#     is tested without a kernel.

import os
import random
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import campaign as K  # noqa: E402
import corpus as C  # noqa: E402
import mutate as X  # noqa: E402

u3 = X.u3
FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


class Mock:
    """Simulated kernel: each call contributes edges derived from its syscall and
    operands; every program shares a spawn/exec set; run 0 of each program adds
    one edge the other runs do not (noise)."""

    def __init__(self):
        self.boots = 0

    @staticmethod
    def edges(text):
        p = u3.deserialize(text)
        e = set(range(100, 160))                         # shared spawn/exec edges
        for c in p.calls:
            key = (c.sys,) + tuple(op[0] if op[0] in ("res", "buf", "path") else op
                                   for op in c.args)
            e.add(1000 + zlib.crc32(repr(key).encode()) % 20000)
        return e

    def run(self, texts, runs, virgin):
        self.boots += 1
        out = []
        for t in texts:
            base = self.edges(t)
            rr = []
            for r in range(runs):
                e = set(base)
                if r == 0:
                    e.add(60000 + sum(t.encode()) % 997)  # seen in one run only
                rr.append({"result": K.CLEAN, "classification": None,
                           "edges": e - virgin, "truncated": False})
            out.append(rr)
        return out


def main():
    # ---- 1. the real boot: parse, counts, and the noise the filter removes ----
    res = K.parse_camp_log(os.path.join(HERE, "fixtures", "camp_sample.log"), 6)
    check(all(len(r) == 2 for r in res), "fixture: expected 6 programs x 2 runs")
    check(all(not run["truncated"] for r in res for run in r), "fixture: truncated ids")
    noisy = sum(1 for r in res if r[0]["edges"] != r[1]["edges"])
    check(noisy >= 1, "fixture: no run-to-run difference — the filter would be untested")
    dropped = sum(len((r[0]["edges"] | r[1]["edges"]) - (r[0]["edges"] & r[1]["edges"]))
                  for r in res)
    check(dropped > 0, "fixture: the 2-run intersection dropped nothing")

    # ---- 2. mutators: valid, annotated, balanced; every operator can survive ----
    rng = random.Random(3)
    parents = [u3.deserialize(X.M.u7_batch.gen_ir(m, s, s * 7919 + 1)) for m in (0, 5)
               for s in range(5)]
    ok, used, survived = 0, {}, {}
    for _ in range(400):
        par, don = parents[rng.randrange(len(parents))], parents[rng.randrange(len(parents))]
        m, names = X.mutant(par, rng, don, don)
        for n in names:
            used[n] = used.get(n, 0) + 1
        if m is None:
            continue
        ok += 1
        for n in names:
            survived[n] = survived.get(n, 0) + 1
        check(X.M.valid(m), "mutant fails the validator")
        check(all(c.expect and c.expect[0] != "any" for c in m.calls), "mutant not annotated")
        end = X._model_end(m)
        check(end is not None and not end.ords and not end.addrs, "mutant leaves fds/maps open")
    check(ok / 400 > 0.5, "valid ratio %.2f too low" % (ok / 400))
    for name, _ in X.OPS:
        check(survived.get(name, 0) > 0, "operator %r never produced a valid mutant" % name)

    # ---- 3. corpus: dedup, provenance, round-trip, crash store separate ----
    a = u3.serialize(parents[0])
    b = a.replace("prng lcg", "prng  lcg") if "prng  lcg" not in a else a + "\n"
    c = C.Corpus()
    check(c.add(C.Entry(a, {"origin": "t", "reasons": ["seed"]})), "dedup: first add")
    check(not c.add(C.Entry(a, {"origin": "t2", "reasons": ["seed"]})), "dedup: same IR twice")
    check(c.add(C.Entry(b, {"origin": "t", "reasons": ["seed"]})), "dedup: dropped a unique IR")
    check(len(c) == 2 and c.entries[C.ir_id(a)].provenance.get("also"),
          "dedup: duplicate provenance not recorded")
    try:
        C.Entry(a, {"origin": "t"})
        check(False, "an entry without keep reasons was accepted")
    except ValueError:
        pass
    tmp = tempfile.mkdtemp(prefix="u8self")
    p1, p2 = os.path.join(tmp, "c1.jsonl"), os.path.join(tmp, "c2.jsonl")
    c.export(p1)
    C.Corpus.load(p1).export(p2)
    check(open(p1).read() == open(p2).read(), "corpus: export/import round-trip differs")
    try:
        bad = open(p1).readline().replace(C.PAYLOAD_FORMAT, "kfuzz-input")
        C.Entry.from_json(bad)
        check(False, "an entry with a foreign payload format was accepted")
    except ValueError:
        pass
    cc = C.CrashCorpus()
    cc.add("sigA", "ORACLE_VIOLATION", C.Entry(a, {"origin": "t", "reasons": ["finding"]}))
    cc.add("sigA", "ORACLE_VIOLATION", C.Entry(a, {"origin": "t", "reasons": ["finding"]}))
    check(len(cc.by_sig["sigA"]["entries"]) == 1, "crash corpus: duplicate kept")

    # ---- 4. campaign on the mock: filter, keep, determinism, pairing ----
    u4gen = os.path.join(X.ROOT, "build", "u4gen")

    def camp(arm, seed):
        k = K.Campaign(arm, seed, 6, 2, Mock(), u4gen)
        k.run(3)
        return k
    g1, g2, r1 = camp("guided", 11), camp("guided", 11), camp("random", 11)
    check(g1.metrics == [dict(m, seconds=g1.metrics[i]["seconds"]) for i, m in enumerate(g2.metrics)],
          "campaign: not deterministic for a fixed meta-seed")
    check([e for e in g1.corpus.order[:6]] == [e for e in r1.corpus.order[:6]],
          "pairing: generation 0 differs between the arms")
    check(all(e < 60000 for e in g1.confirmed), "filter: a single-run edge was confirmed")
    check(g1.metrics[0]["noise_edges"] > 0, "filter: noise not counted")
    check(all(e.provenance.get("origin") and e.provenance.get("reasons")
              for e in g1.corpus.entries.values()), "provenance missing on a corpus entry")
    check(any(e.provenance.get("origin") == "mutation" for e in g1.corpus.entries.values()),
          "guided: no mutant was ever kept")

    # ---- 5. the pre-registered rule ----
    rows = lambda g, r: [dict(meta_seed=s, arm="guided", confirmed_edges=g[s]) for s in range(5)] + \
        [dict(meta_seed=s, arm="random", confirmed_edges=r[s]) for s in range(5)]  # noqa: E731
    v, _, _ = K.compare(rows([200, 210, 205, 220, 215], [100, 105, 102, 98, 101]))
    check(v.startswith("guided > random"), "rule: a clear win was not accepted")
    v, _, _ = K.compare(rows([200, 260, 205, 220, 215], [190, 250, 195, 210, 205]))
    check(not v.startswith("guided > random"), "rule: a win smaller than the spread was accepted")
    v, _, _ = K.compare(rows([200, 210, 90, 80, 215], [100, 105, 102, 98, 101]))
    check(not v.startswith("guided > random"), "rule: 3/5 wins was accepted")

    if FAILS:
        for f in FAILS:
            print("U8 SELFTEST: FAIL — " + f, file=sys.stderr)
        sys.exit(1)
    print("U8 SELFTEST: PASS — real boot parsed (%d noisy programs, %d edges filtered); "
          "%d/400 mutants valid, all balanced, every operator survives; corpus dedup/"
          "provenance/round-trip/crash store; campaign filter/determinism/pairing; "
          "pre-registered rule." % (noisy, dropped, ok))


if __name__ == "__main__":
    main()
