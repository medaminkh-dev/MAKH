#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
#
# MAKH UFUZZ U4 — structural authority: validate the C generator's output.
#
# Runs the host tool build/u4gen over a seed range and feeds EVERY generated
# program through the U3 Python validator (fuzz/u3/u3.py). The C generator runs
# independently in the guest, but its output structure is checked here against
# the exact same contract U3 uses, so a C-port bug can never become silent
# fuzzing noise.
#
# Design constraints (all enforced below):
#   1. EVERY program in the range is validated — not a sample.
#   2. FAIL CLOSED — if u4gen is missing, errors, or any program fails to parse
#      or validate, this exits non-zero. Absence of proof is failure.
#   3. SEED ATTRIBUTION — each program is tagged with the seed that produced it.
#   4. NEGATIVE TESTS — known-bad IR must be REJECTED, else the validation path
#      itself is broken and every "pass" above would be meaningless.
#
#   validate.py [N]     # validate seeds 0..N-1 (default 1024)

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "fuzz", "u3"))
import u3  # noqa: E402

U4GEN = os.path.join(ROOT, "build", "u4gen")


def fail(msg):
    print("U4 GEN-CHECK: FAIL — " + msg, file=sys.stderr)
    sys.exit(1)


def split_programs(text):
    """Split u4gen output on `### seed N` markers into (seed, ir_text) blocks."""
    progs, seed, cur = [], None, []
    for line in text.splitlines():
        if line.startswith("### seed"):
            if seed is not None:
                progs.append((seed, "\n".join(cur)))
            seed, cur = int(line.split()[2]), []
        elif seed is not None:
            cur.append(line)
    if seed is not None:
        progs.append((seed, "\n".join(cur)))
    return progs


def validate_mode(mode, label, n, abi):
    """Sweep seeds 0..n-1 of one generator mode; fail closed on any bad program."""
    try:
        out = subprocess.run([U4GEN, "0", str(n), str(mode)], capture_output=True,
                             text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        fail("u4gen (%s) did not run cleanly: %s" % (label, e))

    progs = split_programs(out.stdout)
    if len(progs) != n:
        fail("%s: expected %d programs, parsed %d (truncated output?)"
             % (label, n, len(progs)))

    for seed, ir in progs:
        try:
            p = u3.deserialize(ir)
        except Exception as e:                       # noqa: BLE001 — fail closed
            fail("%s seed %d produced un-parseable IR: %s" % (label, seed, e))
        try:
            u3.validate(p, abi)
        except u3.ValErr as e:
            fail("%s seed %d produced an INVALID program: %s" % (label, seed, e))
        except Exception as e:                       # noqa: BLE001 — fail closed
            fail("%s seed %d: validator raised unexpectedly: %s" % (label, seed, e))
    return n


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 1024
    abi = u3.Abi()

    if not os.path.exists(U4GEN):
        fail("%s is not built (run `make u4-gen-check`)" % os.path.relpath(U4GEN, ROOT))

    # Validate EVERY mode the guest can run (so a C-port bug in any of them is
    # caught by the structural authority, not left as silent fuzzing noise).
    # (mode, label, how many seeds). The fd-exhaustion programs do not depend on
    # the seed, so one instance of each is the whole space.
    modes = [(0, "skeleton", n), (1, "ext2", n), (2, "ext2-direct", n),
             (3, "emfile", 1), (4, "emfile-control", 1),
             (5, "fdops", n), (6, "pipe-rollback", 1)]
    total = sum(validate_mode(mode, label, cnt, abi) for mode, label, cnt in modes)

    # (4) Negative controls: the validator MUST reject these, or the whole check
    # above proves nothing. Each is syntactically parseable but breaks a rule.
    negatives = [
        # use-before-create: write on an fd that was never opened
        ("use-before-create",
         "abi 1\nprng lcg 0\nhandles fd\nbuf 0 8 0000000000000000\n"
         "call write res:0 buf:0 imm:8\n"),
        # imm where a pointer (write's buf) is required
        ("imm-for-pointer",
         "abi 1\nprng lcg 0\nhandles fd\npath 0 /x\n"
         "call open path:0 imm:0 -> fd:0\ncall write res:0 imm:5 imm:8\n"),
        # resource-type mismatch: an addr handle passed to close (wants fd)
        ("res-type-mismatch",
         "abi 1\nprng lcg 0\nhandles addr\n"
         "call mmap imm:0 imm:4096 imm:3 imm:34 -> addr:0\ncall close res:0\n"),
        # a deliberately-bad pointer stands only for a pointer arg, never an fd
        ("bad-for-scalar",
         "abi 1\nprng lcg 0\nhandles -\ncall close bad:0\n"),
        # a baseline slot stands only for a scalar, never a pointer
        ("slot-for-pointer",
         "abi 1\nprng lcg 0\nhandles -\ncall pipe slot:0\n"),
    ]
    for name, ir in negatives:
        try:
            u3.validate(u3.deserialize(ir), abi)
        except u3.ValErr:
            continue                                 # correctly rejected
        except Exception as e:                       # noqa: BLE001
            fail("negative case %r raised a non-ValErr: %s" % (name, e))
        fail("negative case %r was NOT rejected (validation path is broken)" % name)

    print("U4 GEN-CHECK: PASS — %d generated programs (%d modes) all structurally "
          "valid; %d negative cases all rejected." % (total, len(modes), len(negatives)))


if __name__ == "__main__":
    main()
