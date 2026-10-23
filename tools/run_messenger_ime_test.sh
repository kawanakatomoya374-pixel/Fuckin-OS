#!/usr/bin/env bash
# Japanese IME in C-OS Message: boots with the app open, switches the IME on with its button, types romaji, converts
# (Space) and commits (Enter); screenshots /tmp/ime_1.ppm (typing) /tmp/ime_2.ppm (candidates) /tmp/ime_3.ppm (committed).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_SYSMON=14 >/tmp/messenger_build.log 2>&1 || { tail -20 /tmp/messenger_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-messenger.XXXXXX.log); QMP=/tmp/cos_gui_qmp.sock; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "MESSENGER\] listening" "$LOG" 2>/dev/null && break; done
sleep 5
python3 - "$QMP" <<'PY'
import sys, time
sys.path.insert(0, "tools")
from qmp_ui import UI
ui = UI(sys.argv[1])
ox, oy = 144, 146                                   # client origin of the window on the 1024x768 screen
ui.goto(ox + 740, oy + 532); ui.click(); time.sleep(1.5)
ui.type("kyouhaiitenkidesune"); time.sleep(1.5); ui.shot("/tmp/ime_1.ppm")
ui.key("spc"); time.sleep(25); ui.shot("/tmp/ime_2.ppm")          # the first conversion loads the dictionary
ui.key("ret"); time.sleep(1.5)
ui.type("watashiha"); ui.key("spc"); time.sleep(2); ui.key("ret"); time.sleep(1.5); ui.shot("/tmp/ime_3.ppm")
PY
grep -a "MESSENGER\|LAYOUT.*ime" "$LOG" | head
