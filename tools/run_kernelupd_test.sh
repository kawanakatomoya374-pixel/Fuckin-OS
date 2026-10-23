#!/usr/bin/env bash
# Kernel updates (stage 2, docs/app_update.md): a hard-disk image (GPT, ESP with GRUB and kernel slot a) boots; cosupdate fetches a signed kernel over HTTP from the host and the kernel
# stages it in slot b; GRUB starts it once as a trial, it confirms itself; a kernel that starts but never confirms, and one GRUB cannot load, fall back to the previous kernel and are
# not tried again; a tampered kernel and an old one are refused. MODE=bios (default) or uefi; the whole sequence runs for bios, the first three steps for uefi.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
MODE="${MODE:-bios}"
KEY=build/update/kernel_update_key.pem
W=$(mktemp -d /tmp/kupd.XXXXXX); SRV="$W/srv"; mkdir -p "$SRV"; IMG="$W/disk.img"
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
stopvm() { pkill -x qemu-system-x86 2>/dev/null; sleep 2; }
build() {   # SERIAL OUT [NOCONFIRM]
  touch src/kernel/cos_bootupd.c
  make -j"$(nproc)" iso VALIDATION_TESTPROGS=1 KERNEL_SERIAL="$1" ${3:+KUPD_NOCONFIRM=1} >"$W/build.log" 2>&1 || { tail -20 "$W/build.log"; exit 1; }
  cp build/kernel.boot.gz "$2"
}
trap 'stopvm; pkill -f "[h]ttp.server 8124" 2>/dev/null; touch src/kernel/cos_bootupd.c' EXIT
echo "== building the kernels (serial 1000 on the disk, 2000 normal, 3000 never confirms)"
build 1000 "$W/k1.gz"; build 2000 "$W/k2.gz"; build 3000 "$W/k3.gz" 1
python3 - "$W" <<'P'
import sys, os, gzip
w = sys.argv[1]
open(os.path.join(w, "k4.gz"), "wb").write(gzip.compress(os.urandom(4096)))      # a gzip file that is not a kernel: GRUB cannot load it
P
python3 tools/make_hdd_image.py --kernel "$W/k1.gz" --boot-tree iso/boot --out "$IMG" >/dev/null || { echo "FAIL  image"; exit 1; }
make build/storage.img >/dev/null 2>&1
pkill -f "[h]ttp.server 8124" 2>/dev/null; sleep 1
(cd "$SRV" && python3 -m http.server 8124 --bind 127.0.0.1 >"$W/http.log" 2>&1 &)
URL="http://192.168.70.2:8124/"
publish() {   # KERNEL.gz SERIAL [tamper]
  rm -f "$SRV"/*; python3 tools/sign_kernel.py "$1" "$KEY" "$2" "$W/blob.bin" >/dev/null
  if [[ -n "${3:-}" ]]; then python3 - "$W/blob.bin" <<'P'
import sys
f = open(sys.argv[1], "r+b"); f.seek(5000); b = f.read(1); f.seek(5000); f.write(bytes([b[0] ^ 0xFF]))
P
  fi
  python3 tools/mkupdate.py --out "$SRV" --key build/update/update_key.pem --kernel "$W/blob.bin" --kernel-serial "$2" --kernel-label "test-$2" >/dev/null
}
# boot LOG WAIT_PATTERN EXTRA_SECONDS
boot() {
  stopvm; QMP="$W/qmp"; rm -f "$QMP"
  COS_BOOT_DISK="$IMG" COS_DISK_BUS=ide COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$1" COS_QMP="$QMP" setsid tools/run_qemu_c-os.sh --"$MODE" --cpus 4 >/dev/null 2>&1 &
  for _ in $(seq 1 100); do sleep 2; grep -aq "${2:-taskbar launched}" "$1" 2>/dev/null && break; done
  sleep "${3:-8}"
}
guest() { COS_QMP="$W/qmp" python3 -u tools/kupd_peer.py "$L" "$W/qmp" "$@"; }
step() { L="$W/$1.log"; echo "== $2"; }

step s1 "boot kernel 1000 (slot a), install kernel 2000 over HTTP"
publish "$W/k2.gz" 2000
boot "$L"
chk "booted from slot a with serial 1000" "grep -aq 'BOOTUPD\] kernel serial 1000, started from slot a' '$L'"
guest "cosupdate url $URL" "cosupdate status" "cosupdate check" "cosupdate install" >"$W/s1.out"
chk "check offers the kernel"  "grep -aq 'UPDATE\] kernel: serial 1000' '$L' && grep -aq 'UPDATE Kernel: serial 2000' '$L'"
chk "install staged it in the other slot" "grep -aq 'UPDATE\] kernel: installed serial 2000' '$L' && grep -aq 'BOOTUPD\] kernel serial 2000 staged in slot b' '$L'"
stopvm

step s2 "restart: GRUB starts slot b as a trial, it confirms itself"
boot "$L" "BOOTUPD\] confirmed" 3
chk "slot b started with serial 2000" "grep -aq 'BOOTUPD\] kernel serial 2000, started from slot b' '$L'"
chk "it knew it was a trial and confirmed itself" "grep -aq 'BOOTUPD\] this is a trial kernel' '$L' && grep -aq 'BOOTUPD\] confirmed: slot b' '$L'"
stopvm

step s3 "restart again: slot b is the active kernel, no trial"
boot "$L"
chk "slot b, serial 2000, no trial" "grep -aq 'BOOTUPD\] kernel serial 2000, started from slot b' '$L' && ! grep -aq 'this is a trial kernel' '$L'"
guest "cosupdate status" "cosupdate check" >/dev/null
chk "the same kernel is not offered again" "! grep -aq 'UPDATE Kernel' '$L'"
if [[ "$MODE" == bios ]]; then
  publish "$W/k2.gz" 2500 tamper
  guest "cosupdate install" >/dev/null
  chk "a kernel with a broken signature is refused (nothing written)" "grep -aq 'UPDATE\] kernel: refused by the kernel' '$L' && ! grep -aq 'serial 2500 staged' '$L'"
  publish "$W/k3.gz" 3000
  guest "cosupdate url $URL" "cosupdate install" >/dev/null
  chk "kernel 3000 (never confirms) staged in slot a" "grep -aq 'BOOTUPD\] kernel serial 3000 staged in slot a' '$L'"
  stopvm

  step s4 "restart: kernel 3000 starts as a trial and never confirms"
  boot "$L" "taskbar launched" 20
  chk "kernel 3000 ran as a trial" "grep -aq 'BOOTUPD\] kernel serial 3000, started from slot a' '$L' && grep -aq 'test build: never confirms' '$L'"
  stopvm

  step s5 "restart: the unconfirmed trial is dropped, slot b runs again, 3000 is not tried again"
  boot "$L"
  chk "back on slot b (serial 2000)" "grep -aq 'BOOTUPD\] kernel serial 2000, started from slot b' '$L'"
  chk "the failed update is noted" "grep -aq 'did not start: back on the previous kernel; serials up to 3000' '$L'"
  guest "cosupdate check" >/dev/null
  chk "kernel 3000 is not offered any more" "! grep -aq 'UPDATE Kernel' '$L'"
  stopvm

  step s6 "a validly signed 'kernel' GRUB cannot load: the fallback entry starts the previous kernel"
  python3 tools/sign_kernel.py "$W/k4.gz" "$KEY" 4000 "$W/blob4.bin" >/dev/null; rm -f "$SRV"/*
  python3 tools/mkupdate.py --out "$SRV" --key build/update/update_key.pem --kernel "$W/blob4.bin" --kernel-serial 4000 --kernel-label bad >/dev/null
  boot "$L"; guest "cosupdate url $URL" "cosupdate install" >/dev/null
  chk "the broken kernel was staged (it is signed)" "grep -aq 'BOOTUPD\] kernel serial 4000 staged' '$L'"
  stopvm
  step s7 "restart: GRUB fails to load the trial kernel and falls back"
  boot "$L"
  chk "the previous kernel (slot b, 2000) came up by itself" "grep -aq 'BOOTUPD\] kernel serial 2000, started from slot b' '$L' && grep -aq 'taskbar launched' '$L'"
  stopvm
  step s8 "restart: the broken trial is recorded as failed"
  boot "$L"
  chk "serials up to 4000 are not tried again" "grep -aq 'did not start: back on the previous kernel; serials up to 4000' '$L'"
fi
stopvm
[[ $FAIL == 0 ]] && echo "kernel update test: ALL PASS ($MODE)" || echo "kernel update test: FAILED ($MODE)"
rm -rf "$W"
exit $FAIL
