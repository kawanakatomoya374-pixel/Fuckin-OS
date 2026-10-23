#!/usr/bin/env bash
# Self-hosting: one C-OS guest runs C-OS Message (VALIDATION_SYSMON=14); the Menu button "Start" starts the server (/bin/CosMsgServer.c-os) and
# points the app at it. Bob on the host joins through a port forward (15151 -> 5151).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/selfhost_build.log 2>&1 || { tail -20 /tmp/selfhost_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-selfhost.XXXXXX.log); QMP=/tmp/cos_gui_qmp.sock; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_HOSTFWD="tcp:127.0.0.1:15151-:5151" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null  && break; done
sleep 5
python3 -u tools/selfhost_peer.py 15151 "$LOG"; RC=$?
python3 tools/qmp_screendump.py "$QMP" /tmp/selfhost.ppm >/dev/null 2>&1
grep -a "COSMSGD\|MESSENGER\] server" "$LOG" | tail -12
exit $RC
