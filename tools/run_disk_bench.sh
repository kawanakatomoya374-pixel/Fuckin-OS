#!/usr/bin/env bash
# Disk benchmark: boots a VALIDATION_DISKBENCH=1 image from a GPT disk on each requested controller (COS_BENCH_BUSES, default
# "ide ahci nvme"), timestamps the kernel's [DISKBENCH] markers on the HOST clock and prints MB/s and IOPS. The scratch
# extent is the last 128 MiB of the data partition, so run it on a throw-away image (this script builds one).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUSES="${COS_BENCH_BUSES:-ide ahci nvme}"
[[ -z "${SKIP_BUILD:-}" ]] && { make -j"$(nproc)" iso VALIDATION_DISKBENCH=1 >/tmp/bench_build.log 2>&1 || { tail -20 /tmp/bench_build.log; exit 1; }; }
IMG=build/bench-disk.img
for BUS in $BUSES; do
  python3 tools/make_hdd_image.py --kernel build/kernel.boot.gz --boot-tree iso/boot --out "$IMG" >/dev/null || exit 1
  LOG=$(mktemp /tmp/cos-bench.XXXXXX.log)
  pkill -x qemu-system-x86 2>/dev/null; sleep 2
  COS_BOOT_DISK="$IMG" COS_DISK_BUS="$BUS" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" \
    setsid tools/run_qemu_c-os.sh --bios --cpus 4 --no-network >/dev/null 2>&1 &
  python3 - "$LOG" "$BUS" <<'P'
import sys, time, re
log, bus = sys.argv[1], sys.argv[2]
pos = 0; start = {}; res = []; t_end = time.time() + 900; failed = False; dev = ''
while time.time() < t_end:
    time.sleep(0.05)
    try: d = open(log, 'rb').read().decode('latin1')
    except FileNotFoundError: continue
    new = d[pos:]; 
    if '\n' not in new: continue
    last = new.rfind('\n') + 1
    now = time.time()
    for line in new[:last].split('\n'):
        m = re.search(r'\[DISKBENCH\] (begin|end) (\w+)(.*)', line)
        if m:
            if m.group(1) == 'begin': start[m.group(2)] = now
            else:
                t = now - start.get(m.group(2), now); extra = m.group(3)
                if 'FAILED' in extra: failed = True
                b = re.search(r'bytes=(\d+)', extra); o = re.search(r'ops=(\d+)', extra)
                if b: res.append((m.group(2), '%7.1f MB/s' % (int(b.group(1)) / 1048576 / t if t else 0), '%.2fs' % t))
                elif o: res.append((m.group(2), '%7.0f IOPS' % (int(o.group(1)) / t if t else 0), '%.2fs' % t))
                else: res.append((m.group(2), '', '%.2fs' % t))
        if 'DISKBENCH] device' in line: dev = line.split('device', 1)[1].strip()
        if 'DISKBENCH] DONE' in line:
            print('== %s (%s)%s' % (bus, dev, '   *** DATA MISMATCH ***' if failed else ''))
            for r in res: print('   %-14s %-14s %s' % r)
            sys.exit(1 if failed else 0)
    pos += last
print('== %s: timed out' % bus); sys.exit(1)
P
  pkill -x qemu-system-x86 2>/dev/null; rm -f "$LOG"
done
