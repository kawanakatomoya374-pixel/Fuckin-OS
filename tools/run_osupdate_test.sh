#!/usr/bin/env bash
# The OS-update notice: the taskbar finds a signed OS release by itself, shows toast / icon / balloon, a click on the icon installs it (tools/osupdate_peer.py).
# packages, rolls back and keeps the update across a reboot (tools/osupdate_peer.py).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_TESTPROGS=1 >/tmp/update_build.log 2>&1 || { tail -20 /tmp/update_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
SRV=$(mktemp -d /tmp/updsrv.XXXXXX); export UPD_SRV="$SRV"
tools/cos-cc -O1 -DVER=2 -o "$SRV/updtest2.c-os" tools/update_test/updtest.c >/dev/null && tools/cos-cc -O1 -DVER=3 -o "$SRV/updtest3.c-os" tools/update_test/updtest.c >/dev/null || { echo "cannot build the test programs"; exit 1; }
LOG=$(mktemp /tmp/cos-update.XXXXXX.log); QMP=/tmp/cos-update.qmp; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
pkill -f "[h]ttp.server 8123" 2>/dev/null || true; sleep 1
pkill -f "[h]ttp.server 8123" 2>/dev/null || true; sleep 1          # a server left from an earlier run would answer with its old package
(cd "$SRV" && python3 -m http.server 8123 --bind 127.0.0.1 >/tmp/update_http.log 2>&1 & echo $! > "$SRV/.pid")
trap 'pkill -x qemu-system-x86 2>/dev/null || true; pkill -f "[h]ttp.server 8123" 2>/dev/null || true; touch src/kernel/cos_system_layout.c' EXIT
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
COS_QMP="$QMP" python3 -u tools/osupdate_peer.py "$LOG" "$QMP"; RC=$?
grep -a "UPDATE\]\|TASKBAR\]" "$LOG" | tail -20
exit $RC
