#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${COS_SMP_MATRIX_OUT:-$ROOT/validation/smp-matrix}"
MEMORY="${COS_MEMORY:-2048M}"
WAIT_SECONDS="${COS_SMP_WAIT:-45}"
BOOT_MODE="${COS_BOOT_MODE:-secure-boot}"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  echo "Usage: COS_SMP_WAIT=45 COS_MEMORY=2048M COS_BOOT_MODE=secure-boot $0"
  echo "Runs Secure Boot UEFI QEMU regression for SMP1 through SMP8 by default."
  echo "Set COS_BOOT_MODE=uefi only for unsigned UEFI diagnosis."
  exit 0
fi

mkdir -p "$OUT_DIR"

for cpus in 1 2 3 4 5 6 7 8; do
  serial="$OUT_DIR/smp${cpus}.serial.log"
  debug="$OUT_DIR/smp${cpus}.qemu-debug.log"
  launch="$OUT_DIR/smp${cpus}.launcher.log"
  qmp="/tmp/cos-smp-matrix-${cpus}.qmp"
  storage="$OUT_DIR/storage${cpus}.img"
  rm -f "$serial" "$debug" "$launch" "$qmp" "$storage"
  cp "$ROOT/build/storage.img" "$storage"
  pkill -x qemu-system-x86_64 2>/dev/null || true
  echo "[SMP-MATRIX] starting cpus=$cpus"
  COS_MEMORY="$MEMORY" COS_STORAGE="$storage" COS_SERIAL="$serial" COS_QMP="$qmp" \
    COS_QEMU_DEBUG_LOG="$debug" COS_VNC="127.0.0.1:$((40 + cpus))" \
    timeout "$((WAIT_SECONDS + 10))" "$ROOT/tools/run_qemu_c-os.sh" "--$BOOT_MODE" --cpus "$cpus" --no-usb \
    >"$launch" 2>&1 &
  qpid=$!
  sleep "$WAIT_SECONDS"
  grep -q "Online CPUs=$cpus" "$serial" || {
    echo "[SMP-MATRIX] FAIL cpus=$cpus: missing Online CPUs marker" >&2
    kill "$qpid" 2>/dev/null || true
    exit 1
  }
  grep -qE "Entering GUI main loop|\[GUI\] Boot complete - drawing desktop" "$serial" || {
    echo "[SMP-MATRIX] FAIL cpus=$cpus: GUI did not start" >&2
    kill "$qpid" 2>/dev/null || true
    exit 1
  }
  if grep -qE '\[PF\]|\[GP\]|PANIC|Out of memory|FATAL: higher' "$serial"; then
    echo "[SMP-MATRIX] FAIL cpus=$cpus: kernel fault/OOM marker" >&2
    kill "$qpid" 2>/dev/null || true
    exit 1
  fi
  echo "[SMP-MATRIX] PASS cpus=$cpus"
  kill "$qpid" 2>/dev/null || true
  wait "$qpid" 2>/dev/null || true
done

echo "[SMP-MATRIX] PASS all CPU counts 1..8"
