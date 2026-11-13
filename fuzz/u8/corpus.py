#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U8 — corpus format and stores.
#
# One entry = a JSON envelope + an IR payload (the U3 IR with U5.3 `=expect`
# annotations). The envelope is the part meant to be shared with other MAKH
# fuzzers (spec §10): an id, what the payload is, where it came from and why it
# was kept. A consumer that does not understand `payload_format` must not run
# the payload; in particular a UFUZZ syscall program is never a KFUZZ target
# input (no adapter exists).
#
# Two stores, never mixed:
#   Corpus       programs kept for coverage / state novelty (guides mutation)
#   CrashCorpus  programs that produced a U6 finding, keyed by signature
#
# Dedup is by the hash of the IR text only, so two different programs are never
# merged; a duplicate's provenance is appended to the existing entry.

import hashlib
import json

SCHEMA = "makh-ufuzz-corpus/1"
PAYLOAD_FORMAT = "u3-ir+u5.3-expect"
REASONS = ("seed", "new-edge", "new-state", "finding")


def ir_id(ir_text):
    return hashlib.sha1(ir_text.encode()).hexdigest()[:16]


class Entry:
    __slots__ = ("id", "ir", "provenance", "new_edges", "features", "generation")

    def __init__(self, ir, provenance, new_edges=0, features=(), generation=0):
        if not provenance or "origin" not in provenance or "reasons" not in provenance:
            raise ValueError("an entry needs provenance (origin, reasons)")
        for r in provenance["reasons"]:
            if r not in REASONS:
                raise ValueError("unknown keep reason %r" % r)
        self.id = ir_id(ir)
        self.ir = ir
        self.provenance = dict(provenance)
        self.new_edges = int(new_edges)
        self.features = sorted(set(features))
        self.generation = int(generation)

    def envelope(self):
        return {"schema": SCHEMA, "id": self.id, "kind": "syscall-program", "target": "makh",
                "payload_format": PAYLOAD_FORMAT, "generation": self.generation,
                "new_edges": self.new_edges, "features": self.features,
                "provenance": self.provenance}

    def to_json(self):
        d = self.envelope()
        d["payload"] = self.ir
        return json.dumps(d, sort_keys=True)

    @staticmethod
    def from_json(line):
        d = json.loads(line)
        if d.get("schema") != SCHEMA or d.get("payload_format") != PAYLOAD_FORMAT:
            raise ValueError("not a %s entry with a %s payload" % (SCHEMA, PAYLOAD_FORMAT))
        e = Entry(d["payload"], d["provenance"], d.get("new_edges", 0), d.get("features", ()),
                  d.get("generation", 0))
        if e.id != d["id"]:
            raise ValueError("entry id does not match its payload")
        return e


class Corpus:
    def __init__(self):
        self.entries = {}          # id -> Entry
        self.order = []            # insertion order (deterministic iteration)

    def __len__(self):
        return len(self.entries)

    def add(self, entry):
        """Add; returns True if it is a new program. A duplicate keeps the first
        entry and records the extra provenance on it."""
        if entry.id in self.entries:
            prev = self.entries[entry.id]
            prev.provenance.setdefault("also", []).append(entry.provenance)
            return False
        self.entries[entry.id] = entry
        self.order.append(entry.id)
        return True

    def export(self, path):
        with open(path, "w") as f:
            for i in self.order:
                f.write(self.entries[i].to_json() + "\n")

    @staticmethod
    def load(path):
        c = Corpus()
        with open(path) as f:
            for line in f:
                if line.strip():
                    c.add(Entry.from_json(line))
        return c


class CrashCorpus:
    """Programs that produced a U6 finding, grouped by signature."""

    def __init__(self):
        self.by_sig = {}           # signature -> {"classification", "entries": [Entry]}

    def add(self, signature, classification, entry):
        slot = self.by_sig.setdefault(signature, {"classification": classification,
                                                  "entries": []})
        if all(e.id != entry.id for e in slot["entries"]):
            slot["entries"].append(entry)

    def export(self, path):
        with open(path, "w") as f:
            for sig in sorted(self.by_sig):
                s = self.by_sig[sig]
                for e in s["entries"]:
                    d = json.loads(e.to_json())
                    d.update(signature=sig, classification=s["classification"])
                    f.write(json.dumps(d, sort_keys=True) + "\n")
