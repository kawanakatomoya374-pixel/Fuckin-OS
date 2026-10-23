#!/usr/bin/env bash
# C-OS Terminal: boots the guest, opens the Terminal from the Start menu and drives it with the keyboard (QMP). Checks (from the serial log) that Enter,
# Backspace and the history work, that a program is started with its arguments and that its output arrives; tcc.c-os compiles and runs a small program.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_TESTPROGS=1 >/tmp/terminal_build.log 2>&1 || { tail -20 /tmp/terminal_build.log; exit 1; }
[ -n "${TERMINAL_TEST_KEEP_IMAGE:-}" ] || { rm -f build/storage.img; make build/storage.img >/dev/null 2>&1; }
LOG=$(mktemp /tmp/cos-terminal.XXXXXX.log); QMP=/tmp/cos-terminal.qmp; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true; [ -n "${TERMINAL_TEST_KEEP_LAYOUT:-}" ] || touch src/kernel/cos_system_layout.c' EXIT
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus ${TERMINAL_TEST_CPUS:-4} ${TERMINAL_TEST_NETWORK---no-network} >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
COS_QMP="$QMP" python3 -u "${PEER:-tools/terminal_peer.py}" "$LOG" "$QMP"; RC=$?
python3 tools/qmp_dump.py "$QMP" > /tmp/stop_regs_terminal.txt 2>&1 || { echo "VM STOPPED: registers in /tmp/stop_regs_terminal.txt"; cp "$LOG" /tmp/stop_serial_terminal.log; }
grep -a "TERMINAL\]" "$LOG" | tail -30
exit $RC
