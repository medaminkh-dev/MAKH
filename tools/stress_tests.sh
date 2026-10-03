#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Amine Khemissi
# stress_tests.sh - boot the test ISO many times (in parallel) to shake out
# timing-dependent races in the concurrency tests. A flaky test is a bug.
#
# usage: tools/stress_tests.sh [runs] [parallel] [iso]
set -u
RUNS=${1:-12}
PAR=${2:-4}
ISO=${3:-makhos-test.iso}
OUT=$(mktemp -d)

run_one() {
    python3 tools/run_tests.py "$ISO" --timeout 180 > "$OUT/run_$1.log" 2>&1
}

i=0
while [ $i -lt "$RUNS" ]; do
    for _ in $(seq 1 "$PAR"); do
        [ $i -ge "$RUNS" ] && break
        i=$((i + 1))
        run_one $i &
    done
    wait
done

fails=$(grep -l "RESULT: FAIL" "$OUT"/*.log | wc -l)
echo "stress: $RUNS runs, $fails failed (logs in $OUT)"
grep -h -E "    FAIL|diag\]|TIMEOUT" "$OUT"/*.log | sort | uniq -c
[ "$fails" -eq 0 ]
