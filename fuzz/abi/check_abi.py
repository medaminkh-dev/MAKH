#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ — U0 ABI consistency checker & documentation generator.
#
# The machine-readable inventory fuzz/abi/makh_syscalls.json is the single
# source of truth for the UFUZZ ABI contract. This tool keeps it honest:
#
#   * it parses the REAL kernel syscall table — numbers from
#     kernel/include/syscall.h and the dispatched `case SYS_*:` labels from
#     kernel/syscall/syscall.c — and checks the JSON against it, so the
#     inventory can never silently drift from what the kernel actually
#     implements (U0 acceptance gate);
#   * it enforces the U0 contract invariants: every pointer argument carries a
#     direction and a size/length rule, and every call states its resource
#     create/transfer/release rules;
#   * it GENERATES docs/UFUZZ_ABI.md from the JSON, and (by default) verifies
#     the committed doc is up to date, so the human-readable view cannot drift
#     from the source of truth either.
#
# Usage:
#   check_abi.py            # full check: JSON vs kernel table, invariants, doc freshness
#   check_abi.py --emit-doc # (re)generate docs/UFUZZ_ABI.md from the JSON
#   check_abi.py --no-doc   # skip the doc-freshness check (JSON/source only)
#
# Exit code 0 = all good; non-zero = a violation was printed. CI gate.

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
JSON_PATH = os.path.join(HERE, "makh_syscalls.json")
HDR_PATH = os.path.join(ROOT, "kernel", "include", "syscall.h")
DISPATCH_PATH = os.path.join(ROOT, "kernel", "syscall", "syscall.c")
DOC_PATH = os.path.join(ROOT, "docs", "UFUZZ_ABI.md")

VALID_IMPL = {"implemented", "partial", "stub", "unsupported"}
VALID_KIND = {"scalar", "ptr"}
VALID_DIR = {"in", "out", "inout"}


def die(errors):
    print("ABI CHECK: FAIL (%d problem(s))\n" % len(errors), file=sys.stderr)
    for e in errors:
        print("  - " + e, file=sys.stderr)
    sys.exit(1)


def parse_header_numbers(path):
    """SYS_* define -> integer number, from kernel/include/syscall.h."""
    nums = {}
    pat = re.compile(r"^\s*#define\s+(SYS_[A-Z0-9_]+)\s+(0[xX][0-9A-Fa-f]+|-?\d+)\b")
    with open(path) as f:
        for line in f:
            m = pat.match(line)
            if m:
                name, val = m.group(1), m.group(2)
                nums[name] = int(val, 16) if val.lower().startswith("0x") else int(val)
    return nums


def parse_dispatched_cases(path):
    """Set of SYS_* that appear as `case SYS_*:` in the dispatcher source.

    This is the authoritative 'implemented' set — everything the kernel will
    route somewhere other than the -ENOSYS default.
    """
    cases = set()
    pat = re.compile(r"\bcase\s+(SYS_[A-Z0-9_]+)\s*:")
    with open(path) as f:
        for line in f:
            for m in pat.finditer(line):
                cases.add(m.group(1))
    return cases


def check(emit_doc, do_doc_check):
    errors = []

    with open(JSON_PATH) as f:
        data = json.load(f)
    syscalls = data.get("syscalls", [])
    hdr = parse_header_numbers(HDR_PATH)
    dispatched = parse_dispatched_cases(DISPATCH_PATH)

    seen_names, seen_nums, json_sys = set(), {}, set()

    for i, sc in enumerate(syscalls):
        tag = sc.get("name", "<#%d>" % i)

        for req in ("name", "sys", "nr", "impl", "args", "ret", "errno", "resources"):
            if req not in sc:
                errors.append("%s: missing required field '%s'" % (tag, req))
        if sc.get("impl") not in VALID_IMPL:
            errors.append("%s: impl '%s' not one of %s" % (tag, sc.get("impl"), sorted(VALID_IMPL)))

        name, sysname, nr = sc.get("name"), sc.get("sys"), sc.get("nr")

        if name in seen_names:
            errors.append("%s: duplicate name" % tag)
        seen_names.add(name)
        if nr in seen_nums:
            errors.append("%s: duplicate number %r (also %s)" % (tag, nr, seen_nums[nr]))
        else:
            seen_nums[nr] = name
        json_sys.add(sysname)

        # (a) number must match the kernel header exactly.
        if sysname not in hdr:
            errors.append("%s: %s is not #defined in kernel/include/syscall.h" % (tag, sysname))
        elif hdr[sysname] != nr:
            errors.append("%s: number %r disagrees with header %s=%d"
                          % (tag, nr, sysname, hdr[sysname]))

        # (b) a documented, supported call must actually be dispatched.
        if sc.get("impl") != "unsupported" and sysname not in dispatched:
            errors.append("%s: impl=%s but %s has no `case` in the dispatcher"
                          % (tag, sc.get("impl"), sysname))
        # ...and an 'unsupported' entry must NOT be dispatched.
        if sc.get("impl") == "unsupported" and sysname in dispatched:
            errors.append("%s: marked unsupported but %s IS dispatched" % (tag, sysname))

        # (d) pointer-argument contract: direction + size are mandatory.
        for a in sc.get("args", []):
            an = a.get("name", "?")
            if a.get("kind") not in VALID_KIND:
                errors.append("%s.%s: kind '%s' invalid" % (tag, an, a.get("kind")))
            if a.get("kind") == "ptr":
                if a.get("dir") not in VALID_DIR:
                    errors.append("%s.%s: pointer arg needs dir in %s" % (tag, an, sorted(VALID_DIR)))
                if not a.get("size"):
                    errors.append("%s.%s: pointer arg needs a size/length rule" % (tag, an))

        # (e) resource ownership must be explicit (arrays may be empty).
        res = sc.get("resources", {})
        for k in ("creates", "consumes", "releases"):
            if not isinstance(res.get(k), list):
                errors.append("%s: resources.%s must be a list" % (tag, k))

    # (c) every dispatched syscall must be documented (no supported-but-undocumented).
    for sysname in sorted(dispatched):
        if sysname not in json_sys:
            errors.append("dispatched %s has no contract in makh_syscalls.json "
                          "(supported syscall without a documented contract)" % sysname)

    # Doc generation / freshness.
    doc = render_doc(data, hdr, dispatched)
    if emit_doc:
        with open(DOC_PATH, "w") as f:
            f.write(doc)
        print("wrote %s (%d syscalls)" % (os.path.relpath(DOC_PATH, ROOT), len(syscalls)))
    elif do_doc_check:
        current = open(DOC_PATH).read() if os.path.exists(DOC_PATH) else ""
        if current != doc:
            errors.append("docs/UFUZZ_ABI.md is stale: regenerate with "
                          "`python3 fuzz/abi/check_abi.py --emit-doc`")

    if errors:
        die(errors)

    counts = {}
    for sc in syscalls:
        counts[sc["impl"]] = counts.get(sc["impl"], 0) + 1
    print("ABI CHECK: PASS — %d syscalls (%s); numbers, dispatch, pointer & "
          "resource contracts all consistent with the kernel table."
          % (len(syscalls), ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts))))


def _args_str(sc):
    parts = []
    for a in sc.get("args", []):
        s = a["name"]
        if a.get("kind") == "ptr":
            s += " (%s %s)" % (a.get("dir", "?"), a.get("size", "?"))
        elif a.get("res"):
            s += " (%s)" % a["res"]
        parts.append(s)
    return ", ".join(parts) if parts else "—"


def _res_str(sc):
    r = sc.get("resources", {})
    bits = []
    for k, sym in (("creates", "+"), ("consumes", "~"), ("releases", "-")):
        for v in r.get(k, []):
            bits.append(sym + v)
    return " ".join(bits) if bits else "—"


def render_doc(data, hdr, dispatched):
    """Deterministically render docs/UFUZZ_ABI.md from the JSON (source of truth)."""
    sc_list = data["syscalls"]
    counts = {}
    for sc in sc_list:
        counts[sc["impl"]] = counts.get(sc["impl"], 0) + 1

    L = []
    L.append("<!-- GENERATED FILE — do not edit by hand.")
    L.append("     Source of truth: fuzz/abi/makh_syscalls.json")
    L.append("     Regenerate: python3 fuzz/abi/check_abi.py --emit-doc -->")
    L.append("")
    L.append("# MAKH UFUZZ — ABI Inventory & Contract (U0)")
    L.append("")
    L.append("ABI version **%d**. Source of truth: `%s`." % (data.get("abi_version", 0), data.get("source_of_truth", "")))
    L.append("")
    L.append("This inventory is what UFUZZ treats as the MAKH Ring-3 syscall ABI. "
             "It is checked against the live kernel table "
             "(`kernel/include/syscall.h` numbers and the dispatched `case SYS_*:` "
             "labels in `kernel/syscall/syscall.c`) by `fuzz/abi/check_abi.py`, which "
             "is a CI gate. A syscall may not be listed as supported without a "
             "contract, every pointer argument carries a direction and a size rule, "
             "and every call states what resources it creates, consumes or releases.")
    L.append("")
    L.append("**Totals:** %d syscalls — %s."
             % (len(sc_list), ", ".join("%d %s" % (counts[k], k) for k in sorted(counts))))
    L.append("")

    # Vocab legend.
    v = data.get("vocab", {})
    L.append("## Legend")
    L.append("")
    L.append("- **impl:** " + "; ".join("`%s` %s" % (k, v["impl"][k]) for k in ("implemented", "partial", "stub", "unsupported") if k in v.get("impl", {})))
    L.append("- **arg notation:** a pointer shows `(dir size)`; a scalar that names a resource shows `(resource)`.")
    L.append("- **dir:** `in` kernel reads · `out` kernel writes · `inout` both.")
    L.append("- **size rules:** `cstr` NUL-terminated · `len:ARG` length from ARG · `struct:NAME` fixed struct · `arr:ARG:ELEM` array · `fixed:N` N bytes · `cstr_arr` NULL-terminated string array.")
    L.append("- **resources:** `+x` creates · `~x` consumes · `-x` releases, where x ∈ {fd, pid, tid, addr, socket, pipe_end, path, buffer, signal_context}.")
    L.append("")

    # Summary table.
    L.append("## Summary")
    L.append("")
    L.append("| nr | name | impl | handler | args | resources |")
    L.append("|---:|------|------|---------|------|-----------|")
    for sc in sorted(sc_list, key=lambda s: s["nr"]):
        L.append("| %d | `%s` | %s | `%s` | %s | %s |"
                 % (sc["nr"], sc["name"], sc["impl"], sc.get("handler", "?"),
                    _args_str(sc).replace("|", "\\|"), _res_str(sc)))
    L.append("")

    # Per-syscall detail.
    L.append("## Contracts")
    L.append("")
    for sc in sc_list:
        L.append("### `%s` — %s (nr %d, %s)" % (sc["name"], sc["sys"], sc["nr"], sc["impl"]))
        L.append("")
        L.append("- **handler:** `%s` (`%s`)" % (sc.get("handler", "?"), sc.get("src", "?")))
        if sc.get("args"):
            L.append("- **arguments:**")
            for a in sc["args"]:
                line = "  - `%s` : `%s`" % (a["name"], a.get("ctype", "?"))
                if a.get("kind") == "ptr":
                    line += " — ptr %s, size `%s`" % (a.get("dir", "?"), a.get("size", "?"))
                if a.get("res"):
                    line += " — resource `%s`" % a["res"]
                if a.get("notes"):
                    line += " — %s" % a["notes"]
                L.append(line)
        else:
            L.append("- **arguments:** none")
        ret = sc.get("ret", {})
        L.append("- **returns:** `%s` — %s" % (ret.get("ctype", "?"), ret.get("meaning", "?")))
        L.append("- **errno:** %s" % (", ".join("`%s`" % e for e in sc.get("errno", [])) or "—"))
        L.append("- **resources:** %s" % _res_str(sc))
        if sc.get("flags"):
            L.append("- **flags:** `%s`" % json.dumps(sc["flags"], ensure_ascii=False))
        if sc.get("side_effects"):
            L.append("- **side effects:** %s" % sc["side_effects"])
        if sc.get("concurrency"):
            L.append("- **concurrency:** %s" % sc["concurrency"])
        if sc.get("notes"):
            L.append("- **notes:** %s" % sc["notes"])
        L.append("")

    return "\n".join(L) + "\n"


def main():
    ap = argparse.ArgumentParser(description="MAKH UFUZZ U0 ABI consistency checker")
    ap.add_argument("--emit-doc", action="store_true", help="(re)generate docs/UFUZZ_ABI.md")
    ap.add_argument("--no-doc", action="store_true", help="skip the doc-freshness check")
    args = ap.parse_args()
    check(emit_doc=args.emit_doc, do_doc_check=not args.no_doc)


if __name__ == "__main__":
    main()
