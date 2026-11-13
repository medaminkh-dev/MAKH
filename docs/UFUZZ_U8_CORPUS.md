<!-- SPDX-License-Identifier: AGPL-3.0-only -->
<!-- Copyright (C) 2026 Amine Khemissi -->

# MAKH UFUZZ — U8: corpus management and structure-aware mutation

**Scope:** spec §4 U8. Keep the programs that teach something, mutate them
structurally, and measure — under a rule fixed before the measurement — whether
that beats fresh random generation. U4 measured that it did not: its "mutation"
only re-drew a selector seed and regenerated the program.
**Depends on:** U3 (IR, validator), U5.3 (deriver), U6 (records, triage,
signatures), U7 (`run-ir`, batch images, crash-aware boots).

All results are `PASS_QEMU`.

## 1. Architecture

```text
 host (Python)                                   guest (one boot per generation)
 ─────────────────────────────────────────       ────────────────────────────────────
 corpus ──► parents ──► mutants                   ufuzzcamp: load /mnt/u8/virgin.bin
   ▲          (validator + deriver,  ──batch.ir──►  for each program, 2 runs:
   │           rebalanced, dropped     +virgin      reset map; run-ir; list edges
   │           if unpredictable)                    new vs the FIXED map
   │                                               UFZ1 tcb/tc/cov/covid (+U6 records)
   └── keep ◄── 2-run filter ◄── parse ◄──────────── serial log
```

The loop is host-driven because only the U5.3 deriver can give an edited
program an independent expectation. The guest never folds the map: the host
owns the global virgin map and intersects the runs of each program.

## 2. Kernel test infrastructure

`kfuzz_cov_fold_new_list(virgin, fold, out, max)` lists, in ascending order,
the edges of the current map that `virgin` has not seen (up to `max`), returns
their total, and folds only when asked. `ufuzz.u8_cov_list_matches_fold`
checks it against `kfuzz_cov_fold_new` on the same map (same count, same
identities, virgin untouched without `fold`). Fail-first: a version that always
folds fails `test_ufuzz.c:778` and `:792`.

`ufuzzcamp` (`makh.ufuzz.camp=<runs>`) loads the 8 KiB virgin map from the batch
image (`tools/u7_batch.py image ... virgin=`), runs every program with coverage
on, and prints `UFZ1 cov n=<tc> new=<count>` plus `UFZ1 covid` lines of edge
ids. Without the cmdline key it does nothing.

## 3. Corpus

`fuzz/u8/corpus.py`. An entry is a JSON envelope — `schema`
`makh-ufuzz-corpus/1`, `id` (hash of the IR), `kind`, `target`,
`payload_format` `u3-ir+u5.3-expect`, `generation`, `new_edges`, `features`,
`provenance` (origin, parents, operators, keep reasons) — plus the IR payload.

- Dedup by IR hash only: two different programs are never merged; a duplicate's
  provenance is appended to the existing entry.
- Every entry must carry provenance with an origin and keep reasons.
- Export/import round-trips byte-identically.
- Findings go to a separate crash corpus keyed by U6 signature.
- Sharing with KFUZZ (spec §10): the envelope fields are common; a consumer
  must refuse a payload format it does not know. A UFUZZ syscall program is
  never handed to KFUZZ as a kernel-target input — no adapter exists
  (`NOT_IMPLEMENTED`).

## 4. Mutation

`fuzz/u8/mutate.py`, all on the IR:

| Operator | What it does |
|---|---|
| `value` | integer boundaries, flags, lengths, offsets from per-(syscall, argument) pools the deriver models |
| `delete` | remove a call and every call using a handle it creates |
| `duplicate` | repeat a call (a creator gets fresh handles) |
| `reorder` | move a call (the validator rejects use-before-create) |
| `alias` | give a call another handle of the same type |
| `pointer` | a pipe/pipe2 output pointer at null / kernel half / non-canonical / unmapped |
| `bytes` | bit flips in a filled buffer |
| `insert` | graft a call from a fresh generator program, with its creators, handles, buffers and paths remapped |
| `splice` | the same from another corpus entry |
| concurrency schedule | `NOT_APPLICABLE` until U9 |

Every mutant (1–3 stacked operators) is balanced twice — by handle, then
semantically from the deriver's end state (an edit can create an fd no handle
names, e.g. `F_GETFL` mutated into `F_DUPFD`) — validated, and re-annotated.
A mutant the deriver cannot fully predict is dropped; the valid-program ratio
is reported per generation.

## 5. Campaign

`fuzz/u8/campaign.py ARM META_SEED --gens G --batch B --runs 2 --tree ROOT
--out DIR`. Generation 0 is the same B fresh programs for both arms of a
meta-seed. Then `random` draws B fresh generator programs (modes 0 and 5) per
generation; `guided` draws B mutants from the corpus (parents weighted by their
new edges and recency, donors from the generator and the corpus).

- **Noise filter:** an edge counts only if both runs of the program hit it;
  the dropped edges are reported (`noise_edges`).
- **Keep:** a clean program with a confirmed globally-new edge or a new
  resource-state feature (syscall, outcome) from the deriver's model.
- **Findings:** any non-clean run, classified by U6, goes to the crash corpus.
- **Metrics per generation:** executions, boots, confirmed kernel edges, new
  edges, noise edges, corpus size, crash signatures, valid ratio, syscalls
  reached, state features, executions to the first new edge after generation 0,
  wall time.

## 6. Pre-registered measurement

Fixed before any measured campaign ran:

- **Budget:** meta-seeds 1–5; 20 generations after generation 0; B = 24
  programs per generation; 2 runs per program; both arms of a pair get the same
  number of programs and runs.
- **Metric:** confirmed kernel edges (2-run filter) at the end of the budget.
- **Rule:** guided > random iff guided has more confirmed edges in ≥ 4 of 5
  pairs **and** the mean difference exceeds the largest within-arm spread
  (max − min of either arm's finals). Otherwise the result is reported as "no
  measurable difference", with the numbers.

Results: §7.

## 7. Results

RESULTS_PENDING
