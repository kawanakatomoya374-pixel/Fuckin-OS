#!/usr/bin/env bash
set -euo pipefail

# C-OS high-load compatibility profile derived from the requested topology.
# C-OS supports 64 logical CPUs and uses EHCI, not xHCI, for USB host tests.
# QEMU 8.2 rejects `-machine ...,thread=multi`; TCG also cannot enforce
# invtsc, x2apic, or tsc-deadline on this host. The supported CPU subset is
# expressed explicitly below instead of silently downgrading an `enforce` run.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${COS_BOOT_MODE:-secure}"
MEMORY="${COS_MEMORY:-8G}"
ISO="${COS_ISO:-$ROOT/C-OS_4.0.8_alpha.iso}"
STORAGE="${COS_STORAGE:-$ROOT/build/stress64-storage.img}"
SERIAL="${COS_SERIAL:-$ROOT/qemu_stress64.serial.log}"
DEBUG="${COS_QEMU_DEBUG_LOG:-$ROOT/qemu_stress64.debug.log}"
QMP="${COS_QMP:-/tmp/cos_qemu_stress64.qmp}"
VNC="${COS_VNC:-127.0.0.1:32}"
MONITOR="${COS_MONITOR:-none}"

usage() {
  cat <<EOF
Usage: $(basename "$0") [--uefi|--secure-boot|--bios]

Runs the 64-vCPU, four-node NUMA, IOMMU, multi-root-port, dual-E1000,
EHCI compatibility profile. Default is Secure Boot UEFI with 8GiB and C-OS GUI.
Secure Boot uses COS_SECUREBOOT_VARS or the enrolled development vars file.
EOF
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --uefi) MODE=uefi; shift;;
    --secure-boot) MODE=secure; shift;;
    --bios) MODE=bios; shift;;
    --help|-h) usage; exit 0;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2;;
  esac
done

[[ -f "$ISO" ]] || { echo "ISO not found: $ISO" >&2; exit 1; }
[[ -f "$STORAGE" ]] || { echo "Storage not found: $STORAGE" >&2; exit 1; }

if [[ "$MODE" == secure ]]; then
  ISO="${COS_SECUREBOOT_ISO:-$ROOT/build/secureboot/C-OS_4.0.8_alpha_secure.iso}"
  VARS_SRC="${COS_SECUREBOOT_VARS:-$ROOT/build/OVMF_VARS_4M.cosdev-secboot.latest.runtime.fd}"
elif [[ "$MODE" == uefi ]]; then
  VARS_SRC="${COS_OVMF_VARS:-$ROOT/build/OVMF_VARS_4M.runtime.fd}"
fi
[[ -f "$ISO" ]] || { echo "ISO not found: $ISO" >&2; exit 1; }

FIRMWARE=()
if [[ "$MODE" != bios ]]; then
  [[ -f "$VARS_SRC" ]] || { echo "OVMF vars not found: $VARS_SRC" >&2; exit 1; }
  VARS="$(mktemp /tmp/cos-stress64-vars.XXXXXX.fd)"
  cp "$VARS_SRC" "$VARS"
  trap 'rm -f "$VARS"' EXIT
  FIRMWARE=(
    -drive "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd"
    -drive "if=pflash,format=raw,file=$VARS"
  )
fi

mkdir -p "$(dirname "$SERIAL")" "$(dirname "$DEBUG")"
rm -f "$SERIAL" "$DEBUG" "$QMP"

ARGS=(
  -machine q35
  -accel tcg,thread=multi
  -cpu max,enforce,+pdpe1gb,-hypervisor
  -smp 64,sockets=4,dies=2,cores=8,threads=1
  -m "$MEMORY",maxmem="$MEMORY"
  -object memory-backend-ram,id=ram0,size=2G,prealloc=on,merge=off
  -object memory-backend-ram,id=ram1,size=2G,prealloc=on,merge=off
  -object memory-backend-ram,id=ram2,size=2G,prealloc=on,merge=off
  -object memory-backend-ram,id=ram3,size=2G,prealloc=on,merge=off
  -numa node,nodeid=0,cpus=0-15,memdev=ram0
  -numa node,nodeid=1,cpus=16-31,memdev=ram1
  -numa node,nodeid=2,cpus=32-47,memdev=ram2
  -numa node,nodeid=3,cpus=48-63,memdev=ram3
  -numa dist,src=0,dst=1,val=40
  -numa dist,src=1,dst=0,val=40
  -numa dist,src=0,dst=2,val=80
  -numa dist,src=2,dst=0,val=80
  -numa dist,src=0,dst=3,val=255
  -numa dist,src=3,dst=0,val=255
  -numa dist,src=1,dst=2,val=80
  -numa dist,src=2,dst=1,val=80
  -numa dist,src=1,dst=3,val=255
  -numa dist,src=3,dst=1,val=255
  -numa dist,src=2,dst=3,val=255
  -numa dist,src=3,dst=2,val=255
  -device intel-iommu,intremap=on,caching-mode=on,device-iotlb=on
  -device pxb-pcie,id=pcie1,bus=pcie.0,bus_nr=64,numa_node=1
  -device pxb-pcie,id=pcie2,bus=pcie.0,bus_nr=96,numa_node=2
  -device pcie-root-port,id=rp0,chassis=1,slot=1
  -device pcie-root-port,id=rp1,chassis=2,slot=2
  -device pcie-root-port,id=rp2,chassis=3,slot=3
  -device pcie-root-port,id=rp3,chassis=4,slot=4
  -device pcie-root-port,id=rp4,bus=pcie1,chassis=5,slot=5
  -device pcie-root-port,id=rp5,bus=pcie1,chassis=6,slot=6
  -device pcie-root-port,id=rp6,bus=pcie2,chassis=7,slot=7
  -device pcie-root-port,id=rp7,bus=pcie2,chassis=8,slot=8
  -drive "if=none,id=disk,file=$STORAGE,format=raw,cache=none,aio=threads,bps_rd=16384,bps_wr=16384,iops_rd=4,iops_wr=4"
  -device ich9-ahci,id=ahci,bus=rp0
  -device ide-hd,drive=disk,bus=ahci.0
  # C-OS implements TinyUSB EHCI host support. The UHCI companion is
  # omitted here because QEMU 8.2 treats an unbound ich9-usb-uhci1 as a PCI
  # device on the last pxb bus; high-speed HID devices work directly on EHCI.
  -device ich9-usb-ehci1,id=ehci,bus=rp1
  -device usb-kbd,bus=ehci.0
  -device usb-mouse,bus=ehci.0
  -netdev user,id=n0
  -device e1000,netdev=n0,bus=rp2,mac=52:54:00:00:00:01
  -netdev user,id=n1
  -device e1000,netdev=n1,bus=rp3,mac=52:54:00:00:00:02
  # pvpanic is an ISA/ACPI notification device in this QEMU profile;
  # it cannot be attached to a PCIe root port.
  -device pvpanic
  -rtc base=utc,clock=vm,driftfix=none
  -global kvm-pit.lost_tick_policy=discard
  -cdrom "$ISO"
  -serial "file:$SERIAL"
  -qmp "unix:$QMP,server=on,wait=off"
  -vnc "$VNC"
  -vga std
  -no-reboot
  -no-shutdown
  -d guest_errors,int
  -D "$DEBUG"
)
ARGS+=("${FIRMWARE[@]}")
if [[ "$MONITOR" == stdio ]]; then
  ARGS+=( -monitor stdio )
elif [[ "$MONITOR" != none ]]; then
  ARGS+=( -monitor "$MONITOR" )
fi

printf 'Launching C-OS stress64: mode=%s memory=%s iso=%s\n' "$MODE" "$MEMORY" "$ISO"
printf 'Serial=%s Debug=%s QMP=%s\n' "$SERIAL" "$DEBUG" "$QMP"
exec qemu-system-x86_64 "${ARGS[@]}"
