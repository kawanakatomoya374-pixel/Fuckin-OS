#!/usr/bin/env bash
# Builds the kernel with the cos_widget demo auto-launched, boots it headless and runs tools/widgets_test.py against
# the real input path (hover/click/typing/slider/list/keys) plus the rectangle-present statistics.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_LAUNCH_WIDGETS=1 >/tmp/widgets_build.log 2>&1 || { tail -20 /tmp/widgets_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-widgets.XXXXXX.log); QMP=/tmp/cos-widgets.qmp
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB="${WIDGETS_USB:-off}" setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
trap 'pkill -x qemu-system-x86 2>/dev/null || true; rm -f "$LOG"' EXIT
for _ in $(seq 1 90); do sleep 2; grep -q "WIDGETS\] ready" "$LOG" 2>/dev/null && break; done
sleep 4
python3 tools/widgets_test.py "$QMP" "$LOG"
