#!/usr/bin/env bash
set -euo pipefail

# C-OS strict QEMU launcher.
# This is the canonical regression entry point: it uses q35, explicit TCG
# multi-thread execution, CPU feature enforcement, strict firmware boot,
# deterministic VM time, guest-error tracing, fixed network parameters, and
# an explicit USB topology.  EHCI is deliberate: C-OS currently implements
# TinyUSB EHCI host support; xHCI tests remain separate until an xHCI HCD is
# implemented and verified.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CPUS="${COS_CPUS:-8}"
MEMORY="${COS_MEMORY:-2048M}"
MODE="${COS_BOOT_MODE:-secure}"
USB="${COS_USB:-all}"
NETWORK="${COS_NETWORK:-on}"
SERIAL="${COS_SERIAL:-$ROOT/qemu_strict_serial.log}"
QMP="${COS_QMP:-/tmp/cos_qemu_strict.qmp}"
DEBUG_LOG="${COS_QEMU_DEBUG_LOG:-$ROOT/qemu_strict_debug.log}"
# Optional QEMU trace specification for focused controller diagnostics.
# Empty by default, so normal strict regressions are unaffected.
QEMU_TRACE="${COS_QEMU_TRACE:-}"
STORAGE="${COS_STORAGE:-$ROOT/build/storage.img}"
VNC="${COS_VNC:-127.0.0.1:20}"
MONITOR="${COS_MONITOR:-none}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

All invocations use the strict q35/TCG/Secure Boot UEFI validation profile
unless --uefi or --bios is explicitly requested. This profile uses 2GiB,
-cpu max,enforce, -accel tcg,thread=multi,
-boot strict=on, deterministic VM RTC, guest_errors/cpu_reset tracing, fixed E1000 user networking, explicit EHCI USB keyboard/mouse devices, and an AC97 device with a host-independent audio backend.

Options:
  --cpus N       Use 1..8 virtual CPUs (default: 8; COS_CPUS also works)
  --bios         Boot the hybrid ISO through legacy BIOS (diagnostic only)
  --uefi         Boot unsigned hybrid ISO through OVMF (diagnostic only)
  --secure-boot  Boot the signed ISO with enrolled development keys
  --no-usb       Do not attach the EHCI USB keyboard/mouse
  --no-network   Do not attach the fixed E1000 user-mode network
  --serial PATH  Serial log path
  --qmp PATH     QMP socket path
  --help         Show this help

Examples:
  ./tools/run_qemu_c-os.sh --cpus 4
  COS_BOOT_MODE=uefi ./tools/run_qemu_c-os.sh --cpus 8
  COS_CPUS=3 COS_MONITOR=stdio ./tools/run_qemu_c-os.sh --uefi
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cpus) CPUS="$2"; shift 2;;
    --bios) MODE="bios"; shift;;
    --uefi) MODE="uefi"; shift;;
    --secure-boot) MODE="secure"; shift;;
    --no-usb) USB="off"; shift;;
    --no-network) NETWORK="off"; shift;;
    --serial) SERIAL="$2"; shift 2;;
    --qmp) QMP="$2"; shift 2;;
    --help|-h) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

if ! [[ "$CPUS" =~ ^[1-8]$ ]]; then
  echo "--cpus must be an integer from 1 through 8" >&2
  exit 2
fi

case "$MODE" in
  bios)
    ISO="$ROOT/C-OS_4.0.8_alpha.iso"
    UEFI_ARGS=()
    ;;
  uefi)
    ISO="$ROOT/C-OS_4.0.8_alpha.iso"
    VARS_SRC="${COS_OVMF_VARS:-$ROOT/build/OVMF_VARS_4M.runtime.fd}"
    ;;
  secure)
    ISO="$ROOT/build/secureboot/C-OS_4.0.8_alpha_secure.iso"
    VARS_SRC="${COS_SECUREBOOT_VARS:-$ROOT/build/OVMF_VARS_4M.cosdev-secboot.latest.runtime.fd}"
    ;;
  *) echo "Unsupported boot mode: $MODE" >&2; exit 2;;
esac

[[ -f "$ISO" ]] || { echo "ISO not found: $ISO" >&2; exit 1; }
[[ -f "$STORAGE" ]] || { echo "Storage image not found: $STORAGE" >&2; exit 1; }

if [[ "$MODE" != bios ]]; then
  [[ -f "${VARS_SRC:?}" ]] || { echo "OVMF variable store not found: $VARS_SRC" >&2; exit 1; }
  # Always run against a disposable copy; do not corrupt Secure Boot enrollment.
  VARS="$(mktemp /tmp/cos-ovmf-vars.XXXXXX.fd)"
  cp "$VARS_SRC" "$VARS"
  trap 'rm -f "$VARS"' EXIT
  UEFI_ARGS=(
    -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd"
    -drive "if=pflash,format=raw,file=$VARS"
  )
fi

mkdir -p "$(dirname "$SERIAL")" "$(dirname "$DEBUG_LOG")"
rm -f "$QMP" "$SERIAL" "$DEBUG_LOG"

# TCG cannot satisfy +invtsc; retain the stricter supported subset with
# -cpu max,enforce. Do not silently request an unsupported feature.
ARGS=(
  -machine q35
  -accel tcg,thread=multi
  -cpu max,enforce
  -smp "$CPUS"
  -m "$MEMORY"
  -boot strict=on
  -rtc base=localtime,clock=vm,driftfix=none
  -global kvm-pit.lost_tick_policy=discard
  -vga std
  -audiodev driver=none,id=snd0
  -device AC97,audiodev=snd0
  -cdrom "$ISO"
  # q35 exposes its default IDE disk through AHCI.  C-OS currently owns a
  # legacy ATA PIO driver, so attach the persistent disk to an explicit
  # PCI IDE controller exposing the compatible primary IDE I/O ports.
  -drive "if=none,id=cos_storage,file=$STORAGE,format=raw,media=disk"
  -device "piix3-ide,id=cos_legacy_ide"
  -device "ide-hd,drive=cos_storage,bus=cos_legacy_ide.0"
  -serial "file:$SERIAL"
  -qmp "unix:$QMP,server=on,wait=off"
  -vnc "$VNC"
  -d cpu_reset,guest_errors
  -D "$DEBUG_LOG"
  -no-reboot
  -no-shutdown
)
ARGS+=("${UEFI_ARGS[@]}")

if [[ -n "$QEMU_TRACE" ]]; then
  ARGS+=( -trace "$QEMU_TRACE" )
fi

if [[ "$MONITOR" == stdio ]]; then
  ARGS+=( -monitor stdio )
elif [[ "$MONITOR" != none ]]; then
  ARGS+=( -monitor "$MONITOR" )
fi

if [[ "$USB" != off ]]; then
  # Keep the validation topology bootable while the C-OS EHCI control-transfer
  # diagnostic is in progress. QEMU presents these HID functions on EHCI. The
  # guest-side enumeration result, rather than a guessed hub topology, decides
  # the next controller-side fix.
  ARGS+=(
    -device ich9-usb-ehci1,id=ehci
    -device usb-kbd,bus=ehci.0,usb_version=2
    # QEMU's mouse otherwise defaults to full-speed and is routed to an UHCI
    # companion HCD that C-OS does not own. Force USB 2.0/high-speed so both
    # boot-protocol HID functions enumerate directly through TinyUSB EHCI.
    -device usb-mouse,bus=ehci.0,usb_version=2
  )
fi
if [[ "$NETWORK" != off ]]; then
  ARGS+=(
    -netdev user,id=n1,net=192.168.70.0/24,dhcpstart=192.168.70.10,restrict=off
    -device e1000,netdev=n1,mac=52:54:00:12:34:56,bus=pcie.0
  )
fi

printf 'Launching C-OS strict profile: mode=%s cpus=%s memory=%s iso=%s\n' "$MODE" "$CPUS" "$MEMORY" "$ISO"
printf 'Serial log: %s\nDebug log: %s\nQMP socket: %s\n' "$SERIAL" "$DEBUG_LOG" "$QMP"
exec qemu-system-x86_64 "${ARGS[@]}"
