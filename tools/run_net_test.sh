#!/usr/bin/env bash
# Network regression: builds a VALIDATION_NET=1 image, serves pattern data from tools/net_test_server.py on the host
# (QEMU user-mode networking exposes the host as the guest's gateway) and checks the [NETTEST] lines the kernel prints:
# ping to the gateway, TCP download/upload on a clean link and with 5% / 15% of frames dropped in both directions.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PORT=18080
# QEMU's slirp pings the host through unprivileged ICMP sockets; allow them where the sandbox disables that.
[[ -w /proc/sys/net/ipv4/ping_group_range ]] && echo "0 2147483647" > /proc/sys/net/ipv4/ping_group_range 2>/dev/null || true
make -j"$(nproc)" iso VALIDATION_NET=1 >/tmp/net_build.log 2>&1 || { tail -20 /tmp/net_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-net.XXXXXX.log); SRV=$(mktemp /tmp/cos-netsrv.XXXXXX.log)
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
python3 tools/net_test_server.py "$PORT" >"$SRV" 2>&1 & SRVPID=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $SRVPID 2>/dev/null || true; rm -f "$LOG" "$SRV"' EXIT
COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 200); do sleep 3; grep -aq "NETTEST\] DONE" "$LOG" 2>/dev/null && break; done
grep -a "NETTEST" "$LOG"
grep -aq "NETTEST\] DONE passed=[0-9]* failed=0" "$LOG"
