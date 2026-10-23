#!/usr/bin/env bash
# Boot-media matrix: C-OS from a hard disk / SSD (legacy IDE, AHCI, NVMe), from a USB stick (xHCI mass storage) and from a CD (the hybrid ISO),
# through legacy BIOS, UEFI and UEFI with Secure Boot enforced (signed standalone GRUB, development certificate enrolled), in QEMU. For each
# combination it boots twice and checks that the kernel found its data partition on that medium, formatted it on the first boot and reused it
# on the second. It also checks that Secure Boot really refuses an unsigned loader, and that a CD with no disk at all still reaches the desktop.
#   MATRIX="bios:ide uefi:usb secure:nvme"  limits it (default: every medium with every firmware mode)    SKIP_BUILD=1  uses the existing build
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
KEY="${SECUREBOOT_KEY:-$HOME/.local/share/c-os-secureboot/C-OS-dev-secureboot.key}"
CERT="${SECUREBOOT_CERT:-$HOME/.local/share/c-os-secureboot/C-OS-dev-secureboot.pem}"
if [[ -z "${MATRIX:-}" ]]; then
  MATRIX=""; for m in bios uefi secure; do for b in ide ahci nvme usb; do MATRIX="$MATRIX $m:$b"; done; done
fi
if [[ -z "${SKIP_BUILD:-}" ]]; then
  make -j"$(nproc)" iso >/tmp/media_build.log 2>&1 || { tail -20 /tmp/media_build.log; exit 1; }
fi
[[ -f build/OVMF_VARS_4M.runtime.fd ]] || cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_4M.runtime.fd
SECURE=0; [[ "$MATRIX" == *secure* ]] && SECURE=1
if [[ $SECURE == 1 ]]; then
  [[ -f "$KEY" && -f "$CERT" ]] || ./tools/generate_secureboot_dev_keys.sh >/dev/null 2>&1
  make secureboot-esp SECUREBOOT_KEY="$KEY" SECUREBOOT_CERT="$CERT" >/tmp/media_secure.log 2>&1 || { tail -20 /tmp/media_secure.log; exit 1; }
  bash tools/make_secboot_vars.sh "$CERT" >/dev/null 2>&1 || { echo "FAIL  secure boot variable store"; exit 1; }
fi
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
stopvm() { pkill -x qemu-system-x86 2>/dev/null; sleep 2; }
# boot IMAGE MODE BUS LOG [WAIT_FOR]
boot() {
  stopvm
  COS_BOOT_DISK="$1" COS_DISK_BUS="$3" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$4" \
    setsid tools/run_qemu_c-os.sh --"$2" --cpus 4 --no-network >/dev/null 2>&1 &
  local pat="${5:-taskbar launched}"
  for _ in $(seq 1 120); do sleep 2; grep -aq "$pat" "$4" 2>/dev/null && break; done
  sleep 6
  stopvm
}
for entry in $MATRIX; do
  MODE="${entry%%:*}"; BUS="${entry##*:}"
  IMG="/tmp/media-$MODE-$BUS.img"
  echo "== $MODE, $BUS"
  if [[ $MODE == secure ]]; then
    python3 tools/make_hdd_image.py --secure-efi build/secureboot/BOOTX64.EFI --cert build/secureboot/C-OS-secureboot-public.cer --out "$IMG" >/dev/null || { echo "FAIL  image build"; FAIL=1; continue; }
  else
    python3 tools/make_hdd_image.py --kernel build/kernel.boot.gz --boot-tree iso/boot --out "$IMG" >/dev/null || { echo "FAIL  image build"; FAIL=1; continue; }
  fi
  L1=$(mktemp /tmp/cos-media1.XXXXXX.log); L2=$(mktemp /tmp/cos-media2.XXXXXX.log)
  boot "$IMG" "$([[ $MODE == secure ]] && echo secure-boot || echo $MODE)" "$BUS" "$L1"
  chk "$MODE/$BUS: firmware loaded the loader, the kernel started and the desktop came up" "grep -aq 'taskbar launched' '$L1'"
  case $BUS in ide) NAME='IDE primary master';; ahci) NAME='AHCI';; nvme) NAME='NVMe';; usb) NAME='USB mass storage';; esac
  chk "$MODE/$BUS: data partition found on the $BUS device" "grep -aq 'GPT disk: C-OS data partition.* on $NAME' '$L1'"
  chk "$MODE/$BUS: first boot formatted it"                 "grep -aq 'formatting persistent storage' '$L1' && grep -aq 'FAT32 filesystem mounted' '$L1'"
  boot "$IMG" "$([[ $MODE == secure ]] && echo secure-boot || echo $MODE)" "$BUS" "$L2"
  chk "$MODE/$BUS: second boot reuses the catalog and the file system" "grep -aq 'Persistent storage catalog loaded' '$L2' && ! grep -aq 'formatting' '$L2'"
  chk "$MODE/$BUS: no kernel fault"                          "! grep -aq 'Unhandled kernel page fault\|stack smashing' '$L1' '$L2'"
  rm -f "$L1" "$L2" "$IMG"
done
if [[ $SECURE == 1 ]]; then
  echo "== secure: an unsigned loader is refused"
  python3 tools/make_hdd_image.py --secure-efi build/secureboot/BOOTX64.unsigned.efi --out /tmp/media-unsigned.img >/dev/null || { echo "FAIL  image build"; FAIL=1; }
  LU=$(mktemp /tmp/cos-media-u.XXXXXX.log)
  boot /tmp/media-unsigned.img secure-boot ide "$LU" 'KERNEL\] C-OS'
  chk "secure: the unsigned BOOTX64.EFI does not run (no kernel output)" "! grep -aq 'KERNEL\] C-OS' '$LU'"
  rm -f "$LU" /tmp/media-unsigned.img
fi
if [[ " $MATRIX " == *" bios:ide "* || -n "${CDTEST:-}" ]]; then
  echo "== CD: the ISO with no disk at all"
  for MODE in bios uefi; do
    LC=$(mktemp /tmp/cos-media-cd.XXXXXX.log); stopvm
    # no COS_BOOT_DISK: the launcher attaches the ISO and a storage image; COS_STORAGE points that at an empty file the kernel cannot use
    : > /tmp/media-empty.img
    COS_STORAGE=/tmp/media-empty.img COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LC" setsid tools/run_qemu_c-os.sh --$MODE --cpus 4 --no-network >/dev/null 2>&1 &
    for _ in $(seq 1 120); do sleep 2; grep -aq 'taskbar launched' "$LC" 2>/dev/null && break; done; sleep 4; stopvm
    chk "CD/$MODE: the desktop comes up from the ISO without a usable disk" "grep -aq 'taskbar launched' '$LC'"
    rm -f "$LC"
  done
fi
exit $FAIL
