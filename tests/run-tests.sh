#!/bin/bash
# run-tests.sh -- compile and run the off-target unit tests with the HOST compiler.
#
# ⚠ These deliberately do NOT use the Retro68 cross-compiler. The point is to prove
# pure logic on a machine where a failure costs a second instead of a reboot, so the
# only files eligible are the dependency-free ones (src/bt_bootreport.c and
# src/bt_hidident.c today).
#
# Run from anywhere:  bluetooth/tests/run-tests.sh

set -u
cd "$(dirname "$0")"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
fail=0

for t in test_*.c; do
    name="${t%.c}"
    # Every dependency-free source the tests may link against goes here.
    if ! cc -Wall -Wextra -O1 -o "$OUT/$name" "$t" ../src/bt_bootreport.c ../src/bt_hidident.c 2>"$OUT/$name.cc"; then
        echo "=== $name: COMPILE FAILED"
        cat "$OUT/$name.cc"
        fail=1
        continue
    fi
    # ⚠ Warnings are shown but do not fail the run: the host compiler is far newer
    # than Retro68's and flags things the target build does not. They are worth
    # reading, not worth blocking on.
    [ -s "$OUT/$name.cc" ] && { echo "=== $name: host-compiler warnings"; cat "$OUT/$name.cc"; }

    echo "=== $name"
    if ! "$OUT/$name"; then fail=1; fi
done

if [ "$fail" -ne 0 ]; then
    echo
    echo "TESTS FAILED"
    exit 1
fi
echo
echo "all tests passed"
