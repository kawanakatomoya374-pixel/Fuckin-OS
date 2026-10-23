#!/usr/bin/env bash
# run_linux_test.sh [command]: boots the VALIDATION_TESTPROGS ISO, opens the Terminal and runs a Linux program from /linux-test (docs/LINUX_COMPAT.md).
# The serial log shows its output as "[TERMINAL] out:" lines and the exit status as "[TERMINAL] exit:".
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
CMD=${1:-"/linux-test/hello one two"}
# LINUX_DISK=img: a storage image with a root file system on it (tools/linux_rootfs_disk.py), used as it is (a copy: the run writes to it)
if [ -n "${LINUX_DISK:-}" ]; then cp --sparse=always "$LINUX_DISK" /tmp/cos-linux-run.img; export COS_STORAGE=/tmp/cos-linux-run.img
else rm -f build/storage.img; make build/storage.img >/tmp/cos-linux-img.log 2>&1 || { tail -5 /tmp/cos-linux-img.log; exit 1; }; fi
LOG=/tmp/cos-linux.log; QMP=/tmp/cos-linux.qmp; rm -f "$QMP" "$LOG"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
NETFLAG=--no-network; [ -n "${LINUX_TEST_NET:-}" ] && NETFLAG=     # LINUX_TEST_NET=1: attach the user-mode network (host = 192.168.70.2)
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus ${LINUX_TEST_CPUS:-2} $NETFLAG >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
if [ -n "${LINUX_TEST_SHOTEVERY:-}" ]; then      # a screendump every N seconds while the test runs: PREFIX_0.ppm, PREFIX_1.ppm ...
  ( i=0; while sleep "$LINUX_TEST_SHOTEVERY"; do python3 - "$QMP" "${LINUX_TEST_SHOTPREFIX:-/tmp/shot}_$i.ppm" <<'PYEOF'
import socket, sys, json, time
try:
    s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.recv(4096)
    s.sendall(b'{"execute":"qmp_capabilities"}\n'); time.sleep(0.2); s.recv(4096)
    s.sendall(json.dumps({"execute": "screendump", "arguments": {"filename": sys.argv[2]}}).encode() + b"\n"); time.sleep(1.0)
except Exception: pass
PYEOF
  i=$((i+1)); done ) &
  SHOTLOOP=$!; trap 'kill $SHOTLOOP 2>/dev/null; pkill -x qemu-system-x86 2>/dev/null || true' EXIT
fi
MC_START=term COS_QMP="$QMP" python3 -u tools/mc_peer.py "$LOG" "$QMP" "$CMD" ${LINUX_TEST_SECS:-20}
[ -n "${LINUX_TEST_SHOT:-}" ] && python3 - "$QMP" "$LINUX_TEST_SHOT" <<'PYEOF'
import socket, sys, json, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\n'); time.sleep(0.3); s.recv(4096)
s.sendall(json.dumps({"execute": "screendump", "arguments": {"filename": sys.argv[2]}}).encode() + b"\n"); time.sleep(1.5); print(s.recv(4096))
PYEOF
[ -n "${LINUX_TEST_DUMP:-}" ] && python3 tools/qmp_dump.py "$QMP" --force > /tmp/cos-linux-regs.txt 2>&1
grep -a "TERMINAL\] out\|TERMINAL\] exit\|\[LINUX\]\|EXCEPTION\|Unhandled" "$LOG" | tail -40
