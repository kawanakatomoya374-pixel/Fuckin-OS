#!/usr/bin/env bash
# Builds with one sysmon view auto-launched (VIEW=1 taskmgr, 2 memory, 3 storage, 4 sysinfo, 5 about, 6 editor, 7 sheet, 8 calc, 9 graph, 10 downloader, 11 games2d, 12 fps, 13 tinygl), boots headless,
# checks the launch/exit log and saves a screenshot to /tmp/sysmon_<view>.ppm.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
VIEW="${1:-1}"
make -j"$(nproc)" iso VALIDATION_SYSMON="$VIEW" >/tmp/sysmon_build.log 2>&1 || { tail -20 /tmp/sysmon_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-sysmon.XXXXXX.log); QMP=/tmp/cos-sysmon.qmp
COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
trap 'pkill -x qemu-system-x86 2>/dev/null || true' EXIT
for _ in $(seq 1 90); do sleep 2; grep -qE "(SYSMON|EDITOR|SHEET|CALC|HTTPDL|GAMES)\] launched" "$LOG" 2>/dev/null && break; done
sleep 6
python3 tools/qmp_screendump.py "$QMP" "/tmp/sysmon_$VIEW.ppm" >/dev/null
grep -E "SYSMON|fault|FAULT|panic|PANIC|denied" "$LOG" | head -20 || true
echo "screenshot: /tmp/sysmon_$VIEW.ppm"
