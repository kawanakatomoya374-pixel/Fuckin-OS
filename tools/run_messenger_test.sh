#!/usr/bin/env bash
# Messenger regression: boots with the Messenger auto-launched (VALIDATION_SYSMON=14), forwards host UDP 18150 to guest 5150,
# lets tools/messenger_host_peer.py send a message (must be ACKed), types a reply into the guest via QMP (the host must
# receive it) and saves a screenshot to /tmp/messenger.ppm.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/messenger_build.log 2>&1 || { tail -20 /tmp/messenger_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-messenger.XXXXXX.log); QMP=/tmp/cos_gui_qmp.sock; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_HOSTFWD="udp:127.0.0.1:18150-:5150" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null && break; done
python3 tools/messenger_host_peer.py 18150 & PEER=$!
sleep 15
python3 tools/qmp_screendump.py "$QMP" /tmp/messenger_1.ppm >/dev/null
python3 tools/qmp_type_text.py "Reply from C-OS" >/dev/null
wait $PEER; RC=$?
sleep 2
python3 tools/qmp_screendump.py "$QMP" /tmp/messenger.ppm >/dev/null
grep -a "MESSENGER\|fault\|FAULT" "$LOG" | head
echo "peer exit code: $RC"
exit $RC
