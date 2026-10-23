#!/usr/bin/env bash
# C-OS Message driven with the mouse only (see tools/gui_peer.py). Python relay server on the host; the guest finds it through the Menu.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/gui_build.log 2>&1 || { tail -20 /tmp/gui_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-gui.XXXXXX.log); QMP=/tmp/cos_gui_qmp.sock; DB=$(mktemp -u /tmp/cosmsg.XXXXXX.db); rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
CRT=$(mktemp -u /tmp/cosmsg.XXXXXX.crt); python3 tools/cosmsg_server.py --port 5151 --bind 127.0.0.1 --db "$DB" --verbose --cert "$CRT" --key "${CRT%.crt}.key" >/tmp/cosmsg_srv.log 2>&1 & SRV=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $SRV 2>/dev/null || true' EXIT
COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null && break; done
sleep 5
MSG_LOG="$LOG" python3 -u tools/gui_peer.py 5151 "$DB"; RC=$?
exit $RC
