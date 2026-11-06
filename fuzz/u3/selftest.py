#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U3 — host-side acceptance tests (no QEMU).
#
# Covers the design's §7 gates that do not need the guest:
#   - generation validity: every generated program passes the validator and
#     most reach a real dependency edge (resource reuse);
#   - validator rejects bad programs (fail-first): an UNTAGGED use-before-create
#     and use-after-release are rejected; the same programs TAGGED are accepted;
#   - serialize round-trip: serialize -> deserialize -> serialize is identical;
#   - the generator stays bound to the ABI (every syscall it emits is
#     MVP-representable per makh_syscalls.json).
# Exit 0 = pass, non-zero = failure. CI gate.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import u3
import gen

FAILS = []


def check(cond, msg):
    if not cond:
        FAILS.append(msg)


def main():
    abi = u3.Abi()

    # (0) the generator's syscalls must all be MVP-representable per the ABI.
    gen_syscalls = ["open", "pipe", "write", "read", "lseek", "close",
                    "mmap", "mprotect", "munmap", "getpid"]
    for name in gen_syscalls:
        check(name in abi.by_name, "generator uses unknown syscall %s" % name)
        check(abi.sc_is_mvp(name),
              "generator syscall %s is not MVP-representable per the ABI" % name)

    # (1) generation validity + reachability over many fixed seeds.
    N = 200
    reach = 0
    for seed in range(N):
        p = gen.generate(abi, seed, steps=8)
        try:
            st = u3.validate(p, abi)
        except u3.ValErr as e:
            FAILS.append("generated program (seed %d) failed validation: %s" % (seed, e))
            continue
        if st["dep_edges"] >= 1:
            reach += 1
        # every generated program must be balanced: no handle left LIVE at end
        # (the validator tracks releases; re-derive liveness here).
        live = _live_handles(p, abi)
        check(not live, "generated program (seed %d) leaves handles live: %r" % (seed, live))
    frac = reach / float(N)
    check(frac >= 0.95, "only %.0f%% of generated programs reached a dependency edge" % (frac * 100))

    # (2) validator rejects bad programs (fail-first), accepts the tagged ones.
    uar = gen.mutate_use_after_release(abi)
    ubc = gen.mutate_use_before_create()
    # tagged -> accepted
    for p, nm in ((uar, "use-after-release"), (ubc, "use-before-create")):
        try:
            u3.validate(p, abi)
        except u3.ValErr as e:
            FAILS.append("tagged %s program was rejected: %s" % (nm, e))
    # untag -> must be rejected
    for p, nm in ((uar, "use-after-release"), (ubc, "use-before-create")):
        for c in p.calls:
            c.tag = None
        rejected = False
        try:
            u3.validate(p, abi)
        except u3.ValErr:
            rejected = True
        check(rejected, "untagged %s program was NOT rejected" % nm)

    # (3) serialize round-trip is identical (generated + the variant probe).
    for seed in range(50):
        p = gen.generate(abi, seed, steps=8)
        s1 = u3.serialize(p)
        s2 = u3.serialize(u3.deserialize(s1))
        check(s1 == s2, "serialize round-trip differs for seed %d" % seed)
    vp = gen.variant_probe(abi)
    check(u3.serialize(vp) == u3.serialize(u3.deserialize(u3.serialize(vp))),
          "serialize round-trip differs for the variant probe")

    # (4) U3.1 variant operands: the variant probe (ioctl TCGETS/TCSETS/
    # TIOCGPGRP/TIOCSPGRP + arch_prctl SET_FS/GET_FS) must validate — the
    # validator resolves each selector to its concrete shape.
    try:
        st = u3.validate(vp, abi)
        check(st["calls"] == 6, "variant probe validated but has %d calls" % st["calls"])
    except u3.ValErr as e:
        FAILS.append("variant probe failed validation: %s" % e)

    # (5) fail-first for variant resolution: a TCGETS whose argp buffer is
    # smaller than struct termios MUST be rejected (the validator resolved the
    # selector to an out ptr of tsz bytes, not a blanket 'variant' pass).
    bad = gen.mutate_variant_bad_argp(abi)
    rejected = False
    try:
        u3.validate(bad, abi)
    except u3.ValErr:
        rejected = True
    check(rejected, "undersized ioctl(TCGETS) argp was NOT rejected")

    # (6) the host struct-size table must agree with the ABI struct_defs, so the
    # validator's min-size can't drift from the inventory's declared layouts.
    for name, sd in abi.struct_defs.items():
        hs = u3.prog_abi_struct_size(name)
        check(hs == sd.get("size"),
              "struct %s size: host table %r != ABI struct_def %r"
              % (name, hs, sd.get("size")))

    if FAILS:
        print("U3 SELFTEST: FAIL (%d)" % len(FAILS))
        for f in FAILS:
            print("  - " + f)
        sys.exit(1)
    print("U3 SELFTEST: PASS — generation valid (%.0f%% reach a dep edge), "
          "validator rejects untagged lifecycle violations, serialize round-trips."
          % (frac * 100))


def _live_handles(p, abi):
    """Handles created but never released (balance check), mirroring the
    validator's release accounting."""
    created, released = set(), set()
    for c in p.calls:
        sc = abi.by_name[c.sys]
        if c.bind:
            for h in c.bind[1]:
                created.add(h)
        for rel in sc.get("resources", {}).get("releases", []):
            for (op, spec) in zip(c.args, sc.get("args", [])):
                if op[0] == u3.RES and spec.get("res") == rel:
                    released.add(op[1])
    return created - released


if __name__ == "__main__":
    main()
