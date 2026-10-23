#!/usr/bin/env bash
# The C-OS Message relay server (userland/programs/cosmsgd) runs inside C-OS under QEMU; the host reaches it through a
# port forward (host 15151 -> guest 5151) and runs tools/test_cosmsgd.py, the same test the Python reference server passes.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_MSGSRV=1 >/tmp/cosmsgd_build.log 2>&1 || { tail -20 /tmp/cosmsgd_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-msgsrv.XXXXXX.log); pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_HOSTFWD="tcp:127.0.0.1:15151-:5151" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MSGSRV\] listening" "$LOG" 2>/dev/null && break; done
python3 tools/test_cosmsgd.py 15151 | grep -v '^PASS'; RC=${PIPESTATUS[0]}
sleep 1; grep -a "MSGSRV" "$LOG" | tail -20
exit $RC
