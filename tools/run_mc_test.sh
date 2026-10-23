#!/usr/bin/env bash
# run_mc_test.sh PROGRAM.c-os [seconds]: boots the built ISO with PROGRAM injected as /mc.c-os, starts it from the Terminal, screenshots in /tmp/mc_N.ppm.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
PROG=$1; SECS=${2:-40}
IMG=build/storage.img; rm -f "$IMG"; make build/storage.img >/dev/null 2>&1 || exit 1
LOG=/tmp/mcb/serial.log; QMP=/tmp/mcb/qmp.sock; rm -f "$QMP" "$LOG"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_STORAGE="$IMG" COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus ${CPUS:-4} --no-network >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
COS_QMP="$QMP" python3 -u tools/mc_peer.py "$LOG" "$QMP" "${MC_CMD:-run /mc.c-os}" "$SECS"
grep -a "MC\|PF\]\|EXC\|panic" "$LOG" | tail -20
