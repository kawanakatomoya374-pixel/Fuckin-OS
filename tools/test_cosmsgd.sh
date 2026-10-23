#!/usr/bin/env bash
# Runs tools/test_cosmsgd.py against the reference (Python) server and the host build of cosmsgd, then restarts cosmsgd
# to check that its state survives.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
tools/cosmsgd_host.sh >/tmp/cosmsgd_hostbuild.log 2>&1 || { tail /tmp/cosmsgd_hostbuild.log; exit 1; }
T=$(mktemp -d); RC=0
pkill -f "cosmsg_server.py --port 5160" 2>/dev/null; pkill -x cosmsgd_host 2>/dev/null; sleep 0.3
echo "=== reference server (Python) ==="
python3 tools/cosmsg_server.py --port 5160 --bind 127.0.0.1 --db $T/ref.db --cert $T/c.crt --key $T/c.key >$T/ref.log 2>&1 & P=$!
sleep 1.5; python3 tools/test_cosmsgd.py 5160 | tee $T/ref.out | grep -v '^PASS' ; [ "${PIPESTATUS[0]}" = 0 ] || RC=1; kill $P 2>/dev/null
echo "=== cosmsgd (host build) ==="
build/cosmsgd_host 5161 $T/data >$T/srv.log 2>&1 & P=$!
sleep 1; python3 tools/test_cosmsgd.py 5161 | tee $T/new.out | grep -v '^PASS'; [ "${PIPESTATUS[0]}" = 0 ] || RC=1
echo "=== restart: state must survive ==="
kill $P; sleep 0.5; rm -rf $T/data2; build/cosmsgd_host 5162 $T/data2 >$T/srv2.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd.py 5162 --phase1 $T/ids.json; kill $P; sleep 0.5
build/cosmsgd_host 5162 $T/data2 >>$T/srv2.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd.py 5162 --phase2 $T/ids.json | tee $T/p2.out; [ "${PIPESTATUS[0]}" = 0 ] || RC=1; kill $P 2>/dev/null
echo "PASS counts: reference $(grep -c '^PASS' $T/ref.out), cosmsgd $(grep -c '^PASS' $T/new.out)"
[ $RC = 0 ] && echo "COSMSGD HOST TEST: ALL PASS" || { echo "COSMSGD HOST TEST: FAILED (logs in $T)"; }
exit $RC
