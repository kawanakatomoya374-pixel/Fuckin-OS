#!/usr/bin/env bash
set -euo pipefail

ROOT="/home/ubuntu/c-os-work/c-os"
PIDFILE="/tmp/cos_gui_qemu.pid"
LOGFILE="$ROOT/qemu_gui_test_serial.log"
QMP="/tmp/cos_gui_qmp.sock"

if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "QEMU is already running: $(cat "$PIDFILE")"
  exit 0
fi
rm -f "$PIDFILE" "$LOGFILE" "$QMP"

qemu-system-x86_64 \
  -machine q35 -cpu max -smp 8 -m 1024M \
  -cdrom "$ROOT/C-OS_4.0.8_alpha.iso" \
  -device e1000,netdev=net0 -netdev user,id=net0 \
  -audiodev driver=none,id=audio -device AC97,audiodev=audio \
  -vnc 127.0.0.1:2 \
  -qmp unix:"$QMP",server=on,wait=off \
  -serial file:"$LOGFILE" -no-reboot -no-shutdown \
  >/tmp/cos_gui_qemu.stderr 2>&1 &

echo $! > "$PIDFILE"
echo "PID=$(cat "$PIDFILE") VNC=127.0.0.1:5902"
