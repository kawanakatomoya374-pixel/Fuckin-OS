#!/usr/bin/env bash
# Kernel modules (.c-osys): boots a validation build (VALIDATION_KMOD=1 adds files the loader must refuse), checks the kernel log for the
# signed example module and the four rejections, then calls the module's system call from a program compiled and run on the guest.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_KMOD=1 >/tmp/kmod_build.log 2>&1 || { tail -c 1500 /tmp/kmod_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-kmod.XXXXXX.log); QMP=/tmp/cos-kmod.qmp; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true; touch src/kernel/cos_system_layout.c' EXIT
COS_EXTRA_ARGS="-device virtio-rng-pci,disable-modern=on,addr=0x6 -device edu,addr=0x5" COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 8
COS_QMP="$QMP" python3 -u tools/kmod_peer.py "$LOG" "$QMP"; RC=$?
python3 tools/qmp_dump.py "$QMP" > /tmp/stop_regs_kmod.txt 2>&1 || { echo "VM STOPPED: registers in /tmp/stop_regs_kmod.txt"; cp "$LOG" /tmp/stop_serial_kmod.log; }
grep -a "KMOD" "$LOG" | cut -c1-200 | head -20
exit $RC
