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
GPU="${COS_GPU:-virtio}"
SERIAL="${COS_SERIAL:-$ROOT/qemu_strict_serial.log}"
QMP="${COS_QMP:-/tmp/cos_qemu_strict.qmp}"
DEBUG_LOG="${COS_QEMU_DEBUG_LOG:-$ROOT/qemu_strict_debug.log}"
# Optional QEMU trace specification for focused controller diagnostics.
# Empty by default, so normal strict regressions are unaffected.
QEMU_TRACE="${COS_QEMU_TRACE:-}"
STORAGE="${COS_STORAGE:-$ROOT/build/storage.img}"
# COS_BOOT_DISK=<image>: boot from a hard-disk image built by tools/make_hdd_image.py (no ISO, GRUB read from the disk).
# COS_DISK_BUS selects how the guest sees that disk: ide (default, the kernel's legacy ATA driver), ahci, nvme or usb (a USB stick on an xHCI controller).
BOOT_DISK="${COS_BOOT_DISK:-}"
DISK_BUS="${COS_DISK_BUS:-ide}"
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
  --virtio-gpu   Use VirtIO-GPU only (default; uses the QEMU-compatible VirtIO GPU scanout)
  --legacy-vga   Use the legacy std VGA device instead
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
    --virtio-gpu) GPU="virtio"; shift;;
    --legacy-vga) GPU="legacy"; shift;;
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
    ISO="$ROOT/C-OS_4.0.9_alpha.iso"
    UEFI_ARGS=()
    ;;
  uefi)
    ISO="$ROOT/C-OS_4.0.9_alpha.iso"
    VARS_SRC="${COS_OVMF_VARS:-$ROOT/build/OVMF_VARS_4M.runtime.fd}"
    ;;
  secure)
    ISO="$ROOT/build/secureboot/C-OS_4.0.8_alpha_secure.iso"
    VARS_SRC="${COS_SECUREBOOT_VARS:-$ROOT/build/OVMF_VARS_4M.cosdev-secboot.latest.runtime.fd}"
    ;;
  *) echo "Unsupported boot mode: $MODE" >&2; exit 2;;
esac

if [[ -n "$BOOT_DISK" ]]; then
  [[ -f "$BOOT_DISK" ]] || { echo "Boot disk image not found: $BOOT_DISK" >&2; exit 1; }
  # --secure-boot with a disk: the image must be one made with make_hdd_image.py --secure-efi (only the signed BOOTX64.EFI on its ESP)
  STORAGE="$BOOT_DISK"
else
  [[ -f "$ISO" ]] || { echo "ISO not found: $ISO" >&2; exit 1; }
  [[ -f "$STORAGE" ]] || { echo "Storage image not found: $STORAGE" >&2; exit 1; }
fi

if [[ "$MODE" != bios ]]; then
  [[ -f "${VARS_SRC:?}" ]] || { echo "OVMF variable store not found: $VARS_SRC" >&2; exit 1; }
  # Always run against a disposable copy; do not corrupt Secure Boot enrollment.
  VARS="$(mktemp /tmp/cos-ovmf-vars.XXXXXX.fd)"
  cp "$VARS_SRC" "$VARS"
  trap 'rm -f "$VARS"' EXIT
  # Secure Boot is only enforced by the secboot firmware build, which needs SMM (variable store protection) and
  # a pflash that only the firmware can write. The plain OVMF_CODE_4M.fd ignores enrolled keys.
  if [[ "${COS_SECURE_FIRMWARE:-$([[ "$MODE" == secure ]] && echo 1 || echo 0)}" == 1 ]]; then
    OVMF_CODE="${COS_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.secboot.fd}"
    SECURE_MACHINE=1
  else
    OVMF_CODE="${COS_OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}"
    SECURE_MACHINE=0
  fi
  [[ -f "$OVMF_CODE" ]] || { echo "OVMF firmware not found: $OVMF_CODE" >&2; exit 1; }
  UEFI_ARGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$VARS"
  )
  if [[ "$SECURE_MACHINE" == 1 ]]; then
    UEFI_ARGS+=( -global driver=cfi.pflash01,property=secure,value=on )
  fi
fi

mkdir -p "$(dirname "$SERIAL")" "$(dirname "$DEBUG_LOG")"
rm -f "$QMP" "$SERIAL" "$DEBUG_LOG"

# TCG cannot satisfy +invtsc; retain the stricter supported subset with
# -cpu max,enforce. Do not silently request an unsupported feature.
if [[ "$GPU" == "legacy" ]]; then
  DISPLAY_ARGS=( -vga std )
else
  # No std VGA device. virtio-vga is the QEMU-compatible VirtIO GPU
  # scanout device; the kernel presents the complete GUI through it.
  VGA_DEV="virtio-vga"
  # COS_GPU_RES=WxH sets the scanout's preferred mode (what UEFI GOP and GRUB's "auto" pick up).
  if [[ -n "${COS_GPU_RES:-}" ]]; then VGA_DEV="virtio-vga,xres=${COS_GPU_RES%x*},yres=${COS_GPU_RES#*x}"; fi
  DISPLAY_ARGS=( -vga none -device "$VGA_DEV" )
fi
MACHINE=q35
[[ "${SECURE_MACHINE:-0}" == 1 ]] && MACHINE="q35,smm=on"
ARGS=(
  -machine "$MACHINE"
  -accel tcg,thread=multi
  -cpu "${COS_CPU:-max,enforce}"
  -smp "$CPUS"
  -m "$MEMORY"
  -boot strict=on
  -rtc base=localtime,clock=vm,driftfix=none
  -global kvm-pit.lost_tick_policy=discard
  "${DISPLAY_ARGS[@]}"
  -audiodev "${COS_AUDIODEV:-driver=none},id=snd0"
  -audiodev driver=none,id=snd_ac97
  -device AC97,audiodev=snd_ac97
  -drive "if=none,id=cos_storage,file=$STORAGE,format=raw,media=disk"
  -serial "file:$SERIAL"
  -qmp "unix:$QMP,server=on,wait=off"
  -vnc "$VNC"
  -d cpu_reset,guest_errors
  -D "$DEBUG_LOG"
  -no-reboot
  -no-shutdown
)
if [[ -z "$BOOT_DISK" ]]; then ARGS+=( -cdrom "$ISO" ); fi
BOOTIDX=""; [[ -n "$BOOT_DISK" ]] && BOOTIDX=",bootindex=0"
case "$DISK_BUS" in
  ide)
    # q35 exposes its default IDE disk through AHCI.  C-OS's legacy ATA PIO driver needs an explicit PCI IDE
    # controller exposing the compatible primary IDE I/O ports.
    ARGS+=( -device "piix3-ide,id=cos_legacy_ide" -device "ide-hd,drive=cos_storage,bus=cos_legacy_ide.0$BOOTIDX" );;
  ahci) ARGS+=( -device "ich9-ahci,id=cos_ahci" -device "ide-hd,drive=cos_storage,bus=cos_ahci.0$BOOTIDX" );;
  nvme) ARGS+=( -device "nvme,id=cos_nvme,serial=cosnvme0,drive=cos_storage$BOOTIDX" );;
  usb) ARGS+=( -device "qemu-xhci,id=cos_xhci" -device "usb-storage,drive=cos_storage,bus=cos_xhci.0$BOOTIDX" );;
  *) echo "COS_DISK_BUS must be ide, ahci, nvme or usb" >&2; exit 2;;
esac
ARGS+=("${UEFI_ARGS[@]}")

if [[ -n "$QEMU_TRACE" ]]; then
  ARGS+=( -trace "$QEMU_TRACE" )
fi

if [[ "$MONITOR" == stdio ]]; then
  ARGS+=( -monitor stdio )
elif [[ "$MONITOR" != none ]]; then
  ARGS+=( -monitor "$MONITOR" )
fi

if [[ "$USB" == xhci || "$USB" == both ]]; then
  # xHCI (USB 3) controller with a keyboard and mouse; COS_USB_EXTRA adds raw -device arguments (e.g. a hub:
  # "-device usb-hub,bus=xhci.0 -device usb-kbd,bus=xhci.0,port=2.1" or "-device usb-storage,drive=...").
  ARGS+=( -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0 )
  if [[ -n "${COS_USB_EXTRA:-}" ]]; then read -r -a XTRA <<<"$COS_USB_EXTRA"; ARGS+=( "${XTRA[@]}" ); fi
fi
if [[ "$USB" != off && "$USB" != xhci ]]; then
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
  # COS_NETDEV replaces the user-mode network with another -netdev (e.g. the IPv6 peer of tools/run_net6_test.sh)
  NETDEV_ARGS=(-netdev "user,id=n1,net=192.168.70.0/24,dhcpstart=192.168.70.10,restrict=off${COS_HOSTFWD:+,hostfwd=$COS_HOSTFWD}")
  if [[ -n "${COS_NETDEV:-}" ]]; then read -r -a NETDEV_ARGS <<<"$COS_NETDEV"; fi
  ARGS+=( "${NETDEV_ARGS[@]}" -device e1000,netdev=n1,mac=52:54:00:12:34:56,bus=pcie.0 )
fi

printf 'Launching C-OS strict profile: mode=%s cpus=%s memory=%s gpu=%s iso=%s\n' "$MODE" "$CPUS" "$MEMORY" "$GPU" "$ISO"
printf 'Serial log: %s\nDebug log: %s\nQMP socket: %s\n' "$SERIAL" "$DEBUG_LOG" "$QMP"
# COS_EXTRA_ARGS: raw extra QEMU arguments (e.g. "-device virtio-rng-pci,disable-modern=on" to give the kernel module tests a device to drive)
if [[ -n "${COS_EXTRA_ARGS:-}" ]]; then read -r -a XARGS <<<"$COS_EXTRA_ARGS"; ARGS+=( "${XARGS[@]}" ); fi
exec qemu-system-x86_64 "${ARGS[@]}"
