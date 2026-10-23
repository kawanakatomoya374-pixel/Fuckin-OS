#!/usr/bin/env bash
# Crash-consistency regression for the catalog store (src/drivers/disk/storage.c): builds a VALIDATION_FS=1 image,
# lets the in-kernel selftest rewrite files with several threads, and kills QEMU with SIGKILL at a random moment
# (a power cut mid-write / mid-commit). The next boot of the SAME disk image must find every file intact (either the
# version before or after the interrupted write) and storage_fsck() clean. Repeats CYCLES times (default 6).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CYCLES="${CYCLES:-6}"
make -j"$(nproc)" iso VALIDATION_FS=1 >/tmp/fs_build.log 2>&1 || { tail -20 /tmp/fs_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
FAIL=0
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
for c in $(seq 1 "$CYCLES"); do
  LOG=$(mktemp /tmp/cos-fs.XXXXXX.log)
  COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
  for _ in $(seq 1 120); do sleep 1; grep -aq "FSTEST\] VERIFY" "$LOG" 2>/dev/null && break; done
  RES=$(grep -a "FSTEST\] VERIFY\|FSTEST\] verify\|FSTEST\] FAIL" "$LOG" | tr '\n' ' ')
  echo "cycle $c: $RES"
  grep -aq "FSTEST\] VERIFY PASS" "$LOG" || FAIL=1
  for _ in $(seq 1 60); do grep -aq "FATCHECK DONE" "$LOG" 2>/dev/null && break; sleep 1; done
  grep -aq "FATCHECK FAIL" "$LOG" && FAIL=1
  [[ $(grep -ac "FATCHECK PASS" "$LOG") -eq 4 ]] || { echo "  FAT32 checker did not pass all 4 checks"; FAIL=1; }
  grep -aq "FSTEST\] FAIL live" "$LOG" && FAIL=1
  # let the writers run for a random while, then pull the plug
  for _ in $(seq 1 60); do sleep 1; [[ $(grep -ac "FSTEST\] writes=" "$LOG") -ge 3 ]] && break; done
  sleep "0.$((RANDOM % 10))$((RANDOM % 10))"; sleep $((RANDOM % 3))
  grep -aq "FSTEST\] FAIL live" "$LOG" && FAIL=1
  pkill -9 -x qemu-system-x86; sleep 3
  rm -f "$LOG"
done
exit $FAIL
