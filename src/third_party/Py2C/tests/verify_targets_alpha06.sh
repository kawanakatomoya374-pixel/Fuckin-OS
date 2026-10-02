#!/bin/sh
# 指定した make ターゲットを順に実行し、結果を build/verify/<target>.log に残す。
#   使い方: sh tests/verify_targets_alpha06.sh test-c99 test-tcc ...
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/verify
rm -f build/verify/done
fail=0
for t in "$@"; do
    start=$(date +%s)
    make "$t" > "build/verify/$t.log" 2>&1
    rc=$?
    dur=$(( $(date +%s) - start ))
    if [ "$rc" -eq 0 ]; then
        printf '%s\n' "PASS  $t (${dur}s)" | tee -a build/verify/summary.txt
    else
        printf '%s\n' "FAIL  $t (rc=$rc, ${dur}s)" | tee -a build/verify/summary.txt
        fail=1
    fi
done
touch build/verify/done
exit $fail
