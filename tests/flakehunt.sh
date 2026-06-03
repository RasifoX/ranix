#!/bin/bash
# Flake hunter — run the test suite N times and report any test that is not
# 100% green across all runs. Intermittent (0 < fails < runs) = flake;
# (fails == runs) = hard failure. This is the systematic replacement for
# chasing flakes by hand one incident at a time ("SIFIR flake toleransı").
#
#   ITERS=30 ARCH=x86_64 ./tests/flakehunt.sh
#   ITERS=20 ARCH=aarch64 BOARD=virt ./tests/flakehunt.sh
set +e
ITERS=${ITERS:-20}
ARCH=${ARCH:-x86_64}
BOARD=${BOARD:-virt}
LOG=build/serial-test.log

declare -A fails
runs=0 nolog=0
# Clean once up front: the per-iter build is incremental, so without this the
# first run can inherit a previous invocation's object files for a DIFFERENT
# arch and silently measure the wrong target (false green).
make clean >/dev/null 2>&1
for i in $(seq 1 "$ITERS"); do
    make ARCH="$ARCH" BOARD="$BOARD" test >/dev/null 2>&1
    if [ ! -f "$LOG" ] || ! grep -qE '^(ALL_PASS|SOME_FAIL)' "$LOG"; then
        nolog=$((nolog + 1)); echo "run $i/$ITERS: NO RESULT (build/boot/hang)"; continue
    fi
    runs=$((runs + 1))
    p=$(grep -c '^PASS:' "$LOG"); f=$(grep -c '^FAIL:' "$LOG")
    echo "run $i/$ITERS: $p pass, $f fail"
    while read -r name; do
        [ -n "$name" ] && fails["$name"]=$(( ${fails["$name"]:-0} + 1 ))
    done < <(grep '^FAIL:' "$LOG" | sed 's/^FAIL: //; s/ (expected.*//')
done

echo "=== flake report: $runs completed runs, $nolog no-result ==="
if [ ${#fails[@]} -eq 0 ] && [ "$nolog" -eq 0 ]; then
    echo "ALL GREEN across $runs runs — no flakes, no hangs."
else
    for k in "${!fails[@]}"; do
        c=${fails[$k]}
        if [ "$c" -eq "$runs" ]; then echo "  HARD  $k: $c/$runs"; else echo "  FLAKE $k: $c/$runs"; fi
    done
    [ "$nolog" -gt 0 ] && echo "  NO-RESULT (hang/build): $nolog/$ITERS"
fi
