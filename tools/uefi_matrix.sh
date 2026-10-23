#!/usr/bin/env bash
# UEFI boot validation matrix (needs OVMF, qemu, and for the Secure Boot rows: virt-fw-vars + the signed ISO from
# `make secureboot-iso`). Each row boots, waits for the desktop (GUI frame counters on the serial log) and fails on a
# kernel page fault / panic. Secure Boot rows also check that an UNSIGNED ISO is refused by the enforcing firmware.
#   tools/uefi_matrix.sh            run everything that can run on this machine
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
FAIL=0
LOG=$(mktemp /tmp/cos-uefi-matrix.XXXXXX.log)
VARS_SB="$ROOT/build/OVMF_VARS_4M.cosdev-secboot.latest.runtime.fd"
[[ -f build/OVMF_VARS_4M.runtime.fd ]] || cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_4M.runtime.fd

boot() {  # label, expect(up|denied), env..., -- run args...
  local label="$1" expect="$2"; shift 2
  local envs=(); while [[ "$1" != "--" ]]; do envs+=("$1"); shift; done; shift
  rm -f "$LOG"
  env COS_VNC="127.0.0.1:$((30 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="/tmp/cos-matrix.qmp" "${envs[@]}" \
      setsid tools/run_qemu_c-os.sh "$@" >/dev/null 2>&1 &
  local pid=$!
  local ok=0
  for _ in $(seq 1 90); do
    sleep 2
    # "up": the kernel reached its last boot phase and the desktop is starting; wait a few more seconds so a late
    # fault (e.g. the first process touching a device mapping) is still caught.
    if [[ "$expect" == up ]] && grep -q "BOOTSPLASH\] launched" "$LOG" 2>/dev/null && grep -q "ACPI\] tables discovered" "$LOG" 2>/dev/null; then sleep 25; ok=1; break; fi
    if [[ "$expect" == denied ]] && grep -q "Access Denied" "$LOG" 2>/dev/null; then ok=1; break; fi
    grep -q "Unhandled kernel page fault\|KERNEL PANIC" "$LOG" 2>/dev/null && break
  done
  pkill -x qemu-system-x86 2>/dev/null; wait "$pid" 2>/dev/null
  if grep -q "Unhandled kernel page fault\|KERNEL PANIC" "$LOG" 2>/dev/null; then ok=0; fi
  if [[ $ok == 1 ]]; then echo "PASS  $label"; else echo "FAIL  $label"; FAIL=1; fi
}

for cpus in 1 2 4 8; do boot "uefi cpus=$cpus" up -- --uefi --cpus "$cpus" --no-network --no-usb; done
boot "uefi usb+net"            up -- --uefi --cpus 4
boot "uefi legacy VGA"         up -- --uefi --cpus 4 --no-network --no-usb --legacy-vga
boot "uefi 8 GiB RAM"          up COS_MEMORY=8192M -- --uefi --cpus 4 --no-network --no-usb
boot "bios cpus=4"             up -- --bios --cpus 4 --no-network --no-usb
if [[ -f build/secureboot/C-OS_4.0.8_alpha_secure.iso && -f "$VARS_SB" ]]; then
  boot "secure boot (signed ISO)"        up     -- --secure-boot --cpus 4 --no-network --no-usb
  boot "secure boot rejects unsigned ISO" denied COS_SECURE_FIRMWARE=1 COS_OVMF_VARS="$VARS_SB" -- --uefi --cpus 4 --no-network --no-usb
else
  echo "SKIP  secure boot rows (build/secureboot ISO or $VARS_SB missing; see tools/make_secboot_vars.sh)"
fi
rm -f "$LOG"
exit $FAIL
