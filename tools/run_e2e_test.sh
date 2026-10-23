#!/usr/bin/env bash
# End-to-end encryption: the guest (C-OS Message in QEMU) and tools/e2e_peer.py (own keys) talk through the reference server on the host;
# the peer checks that the server only holds ciphertext, safety numbers, downgrade refusal, key change handling and encrypted attachments.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/messenger_build.log 2>&1 || { tail -20 /tmp/messenger_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-messenger.XXXXXX.log); QMP=/tmp/cos_gui_qmp.sock; DB=$(mktemp -u /tmp/cosmsg.XXXXXX.db); rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
CRT=$(mktemp -u /tmp/cosmsg.XXXXXX.crt); python3 ${SERVER_PY:-tools/cosmsg_server.py} --port 5151 --bind 127.0.0.1 --db "$DB" --verbose --cert "$CRT" --key "${CRT%.crt}.key" >/tmp/cosmsg_srv.log 2>&1 & SRV=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $SRV 2>/dev/null || true' EXIT
COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null && break; done
sleep 5
MSG_LOG="$LOG" python3 -u tools/e2e_peer.py 5151 "$DB" "$LOG" 2>&1 | tee /tmp/e2e_peer.log; RC=${PIPESTATUS[0]}
sleep 2
python3 tools/qmp_screendump.py "$QMP" /tmp/e2e.ppm >/dev/null
echo "peer exit code: $RC"
exit $RC
