#!/bin/sh
# One-command test runner for the C-OS image decoders (host build, ASan+UBSan).
#   tools/imgtest/run.sh            full run
#   FUZZ_SECONDS=60 tools/imgtest/run.sh    longer fuzzing (default 20 s per run, plus truncation of every seed)
# Needs gcc, python3, numpy, Pillow.  Never touches the kernel build tree.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
export IMGTEST_BUILD=${IMGTEST_BUILD:-/tmp/imgtest-build}
export IMGTEST_TMP=${IMGTEST_TMP:-/tmp/imgtest-work}
export PYTHONPATH=$HERE
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1
FUZZ_SECONDS=${FUZZ_SECONDS:-20}
mkdir -p "$IMGTEST_TMP"
FAIL=0
echo "== build"; "$HERE/build.sh" "$IMGTEST_BUILD" || { echo "BUILD FAILED"; exit 1; }
for t in test_png test_jpeg test_bmp; do
  echo "== $t"
  python3 -W ignore "$HERE/$t.py" > "$IMGTEST_TMP/$t.log" 2>&1; rc=$?
  grep -v '^Caution' "$IMGTEST_TMP/$t.log" | tail -12
  [ $rc = 0 ] || { echo "FAILED: $t"; FAIL=1; }
done
echo "== fuzz (ASan+UBSan)"
SEEDS=$IMGTEST_TMP/seeds; python3 "$HERE/gen_seeds.py" "$SEEDS" || FAIL=1
"$IMGTEST_BUILD/fuzz" -n 3000 -t "$FUZZ_SECONDS" -s 1 "$SEEDS"/* || FAIL=1
echo "== speed (-O2, host)"
python3 "$HERE/gen_big.py" "$IMGTEST_TMP/big" && for f in big.jpg big_prog.jpg big.png big.bmp; do
  "$IMGTEST_BUILD/bench" "$IMGTEST_TMP/big/$f" 3; "$IMGTEST_BUILD/bench" "$IMGTEST_TMP/big/$f" 3 fit; done
if [ $FAIL = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit $FAIL
