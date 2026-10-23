#!/usr/bin/env bash
# Ring-3 socket API regression: boots a VALIDATION_NETSOCK=1 image with the host's port 18090 forwarded to guest port 7000,
# serves tools/net_test_server.py (TCP service + UDP echo) and tools/netsock_host.py (connects to the guest's server), and
# checks that the guest prints [NETSOCK] DONE failed=0.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PORT=18080; FWD=18090
make -j"$(nproc)" iso VALIDATION_NETSOCK=1 >/tmp/netsock_build.log 2>&1 || { tail -20 /tmp/netsock_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-netsock.XXXXXX.log)
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
python3 tools/net_test_server.py "$PORT" >/tmp/netsock_srv.log 2>&1 & SRV=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $SRV 2>/dev/null || true' EXIT
COS_HOSTFWD="tcp:127.0.0.1:$FWD-:7000" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "NETSOCK\] listening" "$LOG" 2>/dev/null && break; done
python3 tools/netsock_host.py "$FWD"; HOSTRC=$?
for _ in $(seq 1 60); do sleep 2; grep -aq "NETSOCK\] DONE" "$LOG" 2>/dev/null && break; done
grep -a "NETSOCK" "$LOG"
echo "host side exit code: $HOSTRC"
grep -aq "NETSOCK\] DONE failed=0" "$LOG" && [ "$HOSTRC" = 0 ]
