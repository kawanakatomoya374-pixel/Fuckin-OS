#!/usr/bin/env bash
# Abuse limits and crash safety of the C server (host build): tools/test_cosmsgd_limits.py, and a state file that was only half renamed.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
tools/cosmsgd_host.sh >/tmp/cosmsgd_hostbuild.log 2>&1 || { tail /tmp/cosmsgd_hostbuild.log; exit 1; }
T=$(mktemp -d); RC=0; pkill -x cosmsgd_host 2>/dev/null; sleep 0.3
build/cosmsgd_host 5171 $T/data >$T/srv.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd_limits.py 5171 || RC=1
kill $P 2>/dev/null; sleep 0.5
echo "=== crash between removing the old state file and renaming the new one ==="
rm -rf $T/d2; build/cosmsgd_host 5172 $T/d2 >$T/s2.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd.py 5172 --phase1 $T/ids.json; kill $P; sleep 0.5
mv $T/d2/state.txt $T/d2/state.txt.new                                   # what a crash at that moment leaves
build/cosmsgd_host 5172 $T/d2 >>$T/s2.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd.py 5172 --phase2 $T/ids.json | tee $T/p2.out | grep -c '^PASS' | xargs echo "phase 2 checks passed after the crash:"; grep -q FAIL $T/p2.out && RC=1; kill $P 2>/dev/null
[ $RC = 0 ] && echo "COSMSGD LIMITS TEST: ALL PASS" || echo "COSMSGD LIMITS TEST: FAILED (logs in $T)"
exit $RC
