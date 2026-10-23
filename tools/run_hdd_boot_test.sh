#!/usr/bin/env bash
# Disk-boot regression: builds a GPT disk image (tools/make_hdd_image.py), boots it with no ISO through legacy BIOS and
# through UEFI (OVMF), on the legacy IDE controller by default (COS_DISK_BUS=ahci|nvme for the other controllers), and
# checks that the kernel found its data partition, formatted it on the first boot, kept it on the second boot, and left
# the boot partitions untouched.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUS="${COS_DISK_BUS:-ide}"
IMG="${HDD_IMG:-build/c-os-disk.img}"
[[ -z "${SKIP_BUILD:-}" ]] && { make -j"$(nproc)" iso >/tmp/hdd_build.log 2>&1 || { tail -20 /tmp/hdd_build.log; exit 1; }; }
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
boot() {  # mode, logfile
  pkill -x qemu-system-x86 2>/dev/null; sleep 2
  COS_BOOT_DISK="$IMG" COS_DISK_BUS="$BUS" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$2" \
    setsid tools/run_qemu_c-os.sh --"$1" --cpus 4 --no-network >/dev/null 2>&1 &
  for _ in $(seq 1 100); do sleep 2; grep -aq "Starting scheduler" "$2" 2>/dev/null && break; done
  sleep 8
  pkill -x qemu-system-x86 2>/dev/null; sleep 2
}
boot_header_ok() { python3 - "$IMG" <<'P'
import sys, struct, zlib
d = open(sys.argv[1], 'rb')
d.seek(512); h = d.read(92)
ok = h[:8] == b'EFI PART'
d.seek(2048 * 512); core = d.read(512)
ok = ok and any(core)
sys.exit(0 if ok else 1)
P
}
for MODE in bios uefi; do
  echo "== $MODE, disk bus $BUS"
  python3 tools/make_hdd_image.py --kernel build/kernel.boot.gz --boot-tree iso/boot --out "$IMG" >/dev/null || { echo "FAIL  image build"; exit 1; }
  L1=$(mktemp /tmp/cos-hdd1.XXXXXX.log); L2=$(mktemp /tmp/cos-hdd2.XXXXXX.log)
  boot "$MODE" "$L1"
  chk "$MODE: GRUB loaded the kernel from the disk"        "grep -aq 'Starting scheduler' '$L1'"
  chk "$MODE: data partition found via GPT"                "grep -aq 'GPT disk: C-OS data partition' '$L1'"
  chk "$MODE: first boot formatted the data partition"     "grep -aq 'formatting persistent storage' '$L1' && grep -aq 'FAT32 filesystem mounted' '$L1'"
  chk "$MODE: boot partitions intact after first boot"     "boot_header_ok"
  boot "$MODE" "$L2"
  chk "$MODE: second boot reuses the catalog"              "grep -aq 'Persistent storage catalog loaded' '$L2' && ! grep -aq 'formatting persistent storage' '$L2'"
  chk "$MODE: second boot reuses the FAT32 filesystem"     "grep -aq 'FAT32 filesystem mounted' '$L2' && ! grep -aq 'formatting' '$L2'"
  chk "$MODE: no kernel fault"                             "! grep -aq 'Unhandled kernel page fault\|stack smashing' '$L1' '$L2'"
  rm -f "$L1" "$L2"
done
exit $FAIL
