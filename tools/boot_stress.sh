#!/usr/bin/env bash
# boot_stress.sh [N] - boots the guest N times (default 20) and checks, a while after the desktop is up, that the VM is still running. On a stop (a guest
# triple fault) the registers of all CPUs are saved to /tmp/boot_stress_fail_<i>.txt together with the serial log, and the loop ends.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
N=${1:-20}
for i in $(seq 1 "$N"); do
  rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
  LOG=/tmp/boot_stress_$i.log; QMP=/tmp/boot_stress.qmp; rm -f "$LOG" "$QMP"
  pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
  COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off COS_QEMU_DEBUG_LOG=/tmp/boot_stress_dbg.log setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
  for _ in $(seq 1 60); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
  sleep 20
  if ! python3 tools/qmp_dump.py "$QMP" > /tmp/boot_stress_state.txt 2>&1; then
    cp /tmp/boot_stress_state.txt /tmp/boot_stress_fail_$i.txt; cp "$LOG" /tmp/boot_stress_fail_$i.log; echo "boot $i: STOPPED (see /tmp/boot_stress_fail_$i.txt)"; pkill -x qemu-system-x86; exit 1
  fi
  echo "boot $i: ok"; pkill -x qemu-system-x86 2>/dev/null || true; rm -f "$LOG"
done
