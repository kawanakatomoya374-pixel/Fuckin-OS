#!/usr/bin/env bash
# Boots headless, waits for the ring-3 taskbar, then: screenshot, open the Calculator from the taskbar's quick-launch button,
# screenshot, click its window button (minimise), screenshot, click Start (opens the kernel start menu), screenshot.
# Screenshots: /tmp/shell_{1..12}.ppm. Pass "kill" as $1 to also check the fallback by killing the taskbar process.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso >/tmp/shell_build.log 2>&1 || { tail -20 /tmp/shell_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-shell.XXXXXX.log); QMP=/tmp/cos-shell.qmp
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
python3 - "$QMP" <<'PY'
import sys, time
sys.path.insert(0, 'tools')
from qmp_ui import UI
ui = UI(sys.argv[1])
ui.shot('/tmp/shell_1.ppm')
W, H = 1280, 800
import struct
with open('/tmp/shell_1.ppm', 'rb') as f:
    f.readline(); w, h = map(int, f.readline().split()); W, H = w, h
y = H - 22
ui.goto(W - 320 - 76 + 38 + 17 - 38 + 38 - 38, y)   # placeholder, overwritten below
right = W - 330
ui.goto(600, 200); ui.click('right'); time.sleep(4); ui.shot('/tmp/shell_10.ppm')
ui.goto(660, 262); time.sleep(0.3); ui.click(); time.sleep(3); ui.shot('/tmp/shell_11.ppm')
ui.goto(900, 600); ui.click(); time.sleep(2)
ui.goto(right - 38 + 17, y); ui.click(); time.sleep(8); ui.shot('/tmp/shell_2.ppm')
ui.goto(110 + 40, y); ui.click(); time.sleep(3); ui.shot('/tmp/shell_3.ppm')
ui.goto(50, y); ui.click(); time.sleep(4); ui.shot('/tmp/shell_4.ppm')
ui.type('pai'); time.sleep(3); ui.shot('/tmp/shell_5.ppm')
ui.key('ret'); time.sleep(8); ui.shot('/tmp/shell_6.ppm')
ui.goto(700, 600); ui.click('right'); time.sleep(3); ui.shot('/tmp/shell_7.ppm')
ui.goto(600, 650); ui.click(); time.sleep(2)
ui.goto(47, 640); ui.click(); time.sleep(0.2); ui.click(); time.sleep(6); ui.shot('/tmp/shell_8.ppm')
ui.goto(50, y); ui.click(); time.sleep(3); ui.type('anim'); time.sleep(2); ui.key('ret'); time.sleep(0.4); ui.shot('/tmp/shell_9.ppm')
time.sleep(4)
ui.goto(47, 55); ui.click(); time.sleep(0.2); ui.click(); time.sleep(10); ui.shot('/tmp/shell_12.ppm')
PY
grep -E "SHELL|fault|FAULT|panic|PANIC" "$LOG" | head -20 || true
echo "screenshots: /tmp/shell_{1..12}.ppm"
