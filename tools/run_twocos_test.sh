#!/usr/bin/env bash
# Two C-OS machines (QEMU) talking through a C-OS-hosted Message server. VM A (hostfwd 15151 -> 5151) is the server; VM B reaches
# it as 192.168.70.2:15151 (the host as seen from QEMU's user network). See tools/twocos_peer.py.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/twocos_build.log 2>&1 || { tail -20 /tmp/twocos_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
cp build/storage.img /tmp/cos_stA.img; cp build/storage.img /tmp/cos_stB.img
LA=$(mktemp /tmp/cos-A.XXXXXX.log); LB=$(mktemp /tmp/cos-B.XXXXXX.log); QA=/tmp/cos_qmpA.sock; QB=/tmp/cos_qmpB.sock; rm -f $QA $QB
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_STORAGE=/tmp/cos_stA.img COS_HOSTFWD="tcp:127.0.0.1:15151-:5151" COS_USB=off COS_VNC="127.0.0.1:41" COS_SERIAL="$LA" COS_QMP="$QA" COS_QEMU_DEBUG_LOG=/tmp/qdbgA.log setsid tools/run_qemu_c-os.sh --bios --cpus 2 >/dev/null 2>&1 &
COS_STORAGE=/tmp/cos_stB.img COS_USB=off COS_VNC="127.0.0.1:42" COS_SERIAL="$LB" COS_QMP="$QB" COS_QEMU_DEBUG_LOG=/tmp/qdbgB.log setsid tools/run_qemu_c-os.sh --bios --cpus 2 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LA" 2>/dev/null && grep -aq "MESSENGER\] listening" "$LB" 2>/dev/null && break; done
sleep 5
python3 -u tools/twocos_peer.py "$LA" "$LB" "$QA" "$QB"; RC=$?
python3 tools/qmp_screendump.py "$QA" /tmp/twocos_A.ppm >/dev/null 2>&1; python3 tools/qmp_screendump.py "$QB" /tmp/twocos_B.ppm >/dev/null 2>&1
echo "--- A:"; grep -a "COSMSGD\|MESSENGER\] \(server\|received\|upload\)" "$LA" | tail -12; echo "--- B:"; grep -a "MESSENGER\] \(server\|received\|upload\)" "$LB" | tail -8
exit $RC
