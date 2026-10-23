#!/usr/bin/env bash
# Right-click menus in Messenger: a conversation with a sent message is made by tools/messenger_host_peer.py, then tools/ctxmenu_msg_peer.py right-clicks.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/ctxmsg_build.log 2>&1 || { tail -20 /tmp/ctxmsg_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-messenger.XXXXXX.log); QMP=/tmp/cos_ctxmsg_qmp.sock; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_HOSTFWD="udp:127.0.0.1:18150-:5150" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null && break; done
python3 tools/messenger_host_peer.py 18150 & PEER=$!
sleep 15
python3 tools/qmp_type_text.py "Reply from C-OS" >/dev/null
wait $PEER
sleep 3
python3 -u tools/ctxmenu_msg_peer.py "$LOG" "$QMP"; RC=$?
exit $RC
