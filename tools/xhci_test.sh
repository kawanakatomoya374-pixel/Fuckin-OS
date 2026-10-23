#!/usr/bin/env bash
# xHCI regression: boots C-OS on a QEMU q35 machine whose only USB controller is qemu-xhci with a keyboard, a mouse,
# a hub with a keyboard behind it and a SuperSpeed mass-storage device, then checks that every device was brought up
# through the xHCI driver and that hot-unplug / hot-plug work. Boot mode: BIOS by default, XHCI_TEST_MODE=uefi for OVMF.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
MODE="${XHCI_TEST_MODE:-bios}"
IMG=$(mktemp /tmp/cos-xhci-usb.XXXXXX.img); LOG=$(mktemp /tmp/cos-xhci.XXXXXX.log); QMP=/tmp/cos-xhci.qmp
dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null; mkfs.vfat "$IMG" >/dev/null 2>&1 || true
[[ -f build/OVMF_VARS_4M.runtime.fd ]] || cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_4M.runtime.fd
trap 'pkill -x qemu-system-x86 2>/dev/null || true; rm -f "$IMG" "$LOG"' EXIT
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
COS_USB=xhci COS_USB_EXTRA="-device usb-hub,bus=xhci.0,port=3 -device usb-kbd,bus=xhci.0,port=3.1 -drive if=none,id=u0,file=$IMG,format=raw -device usb-storage,id=ustor,bus=xhci.0,drive=u0" \
  COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" \
  setsid tools/run_qemu_c-os.sh --"$MODE" --cpus 4 --no-network >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 3; grep -q "Mass-storage connected" "$LOG" 2>/dev/null && [[ $(grep -c "Keyboard connected" "$LOG") -ge 2 ]] && break; done
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
chk "xHCI controller started"                     "grep -q 'xHCI running' '$LOG'"
chk "keyboard (root port) mounted"                "[[ \$(grep -c 'Keyboard connected' '$LOG') -ge 1 ]]"
chk "keyboard behind a hub mounted"               "[[ \$(grep -c 'Keyboard connected' '$LOG') -ge 2 ]]"
chk "mouse mounted"                               "grep -q 'Mouse connected' '$LOG'"
chk "SuperSpeed mass storage mounted (bulk I/O)"  "grep -q 'Mass-storage connected: blocks=16384 block-size=512' '$LOG'"
python3 - "$QMP" <<'P'
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.recv(4096)
def c(o):
    s.sendall((json.dumps(o) + '\n').encode()); return s.recv(65536)
c({'execute': 'qmp_capabilities'})
c({'execute': 'device_del', 'arguments': {'id': 'ustor'}}); time.sleep(8)
c({'execute': 'device_add', 'arguments': {'driver': 'usb-tablet', 'id': 'tab', 'bus': 'xhci.0'}}); time.sleep(25)
P
chk "hot-unplug reported"                         "grep -q 'Mass-storage disconnected' '$LOG'"
chk "hot-plugged tablet mounted"                  "grep -q 'Tablet / absolute pointer connected' '$LOG'"
chk "no kernel fault"                             "! grep -q 'Unhandled kernel page fault' '$LOG'"
exit $FAIL
