#!/usr/bin/env bash
# IPv6 regression: boots a VALIDATION_NET=2 image whose only network is a QEMU socket netdev wired to tools/ip6_peer.py
# (an independent IPv6 neighbour: router advertisements, neighbour discovery, ICMPv6 echo, a TCP service). Checks SLAAC,
# neighbour discovery, ping6, and TCP over IPv6 on a clean link and with 5% of frames dropped.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PORT=55556
make -j"$(nproc)" iso VALIDATION_NET=2 >/tmp/net6_build.log 2>&1 || { tail -20 /tmp/net6_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-net6.XXXXXX.log); PEER=$(mktemp /tmp/cos-peer.XXXXXX.log)
pkill -x qemu-system-x86 2>/dev/null || true; (fuser -k "$PORT"/tcp 2>/dev/null || true); sleep 2
python3 tools/ip6_peer.py "$PORT" >"$PEER" 2>&1 & PEERPID=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $PEERPID 2>/dev/null || true; rm -f "$LOG" "$PEER"' EXIT
sleep 1
COS_NETDEV="-netdev socket,id=n1,connect=127.0.0.1:$PORT" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" \
  setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 200); do sleep 3; grep -aq "NETTEST\] DONE" "$LOG" 2>/dev/null && break; done
grep -a "NETTEST\|IP6\]" "$LOG"
echo "--- peer:"; tail -4 "$PEER"
grep -aq "IP6\] address is unique" "$LOG" && grep -aq "DAD NS seen" "$PEER" || { echo "FAIL: duplicate address detection did not run"; exit 1; }
RC=0
grep -aq "NETTEST\] DONE passed=[0-9]* failed=0" "$LOG" && ! grep -aq "bad .* checksum" "$PEER" || RC=1
# second boot: the peer claims the guest's address, the kernel must notice and switch IPv6 off
pkill -x qemu-system-x86 2>/dev/null; kill $PEERPID 2>/dev/null; sleep 2; (fuser -k "$PORT"/tcp 2>/dev/null || true)
python3 tools/ip6_peer.py "$PORT" dup >"$PEER" 2>&1 & PEERPID=$!
sleep 1
rm -f "$LOG"
COS_NETDEV="-netdev socket,id=n1,connect=127.0.0.1:$PORT" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" \
  setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 60); do sleep 2; grep -aq "duplicate address detected" "$LOG" 2>/dev/null && break; done
if grep -aq "duplicate address detected" "$LOG"; then echo "PASS  duplicate address detected and IPv6 disabled"; else echo "FAIL  duplicate address not detected"; RC=1; fi
exit $RC
