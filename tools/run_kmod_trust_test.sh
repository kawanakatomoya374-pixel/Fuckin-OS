#!/usr/bin/env bash
# Key separation (.c-osys): a kernel built with KMOD_TRUSTED_LEVEL=2 does not know the level-3 signing key at all, so the SYSTEM-level modules (signed with that key
# by the build) are refused, while the lower levels still load. The next normal build restores the default (level 3).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
make -j"$(nproc)" iso VALIDATION_KMOD=1 KMOD_TRUSTED_LEVEL=2 >/tmp/kmod_trust_build.log 2>&1 || { tail -c 1500 /tmp/kmod_trust_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-kmodtrust.XXXXXX.log); QMP=/tmp/cos-kmodtrust.qmp; rm -f "$QMP"
pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true; touch src/kernel/cos_system_layout.c; rm -f build/kmod_pubkey.h' EXIT
COS_EXTRA_ARGS="-device virtio-rng-pci,disable-modern=on,addr=0x6" COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" COS_QMP="$QMP" COS_USB=off setsid tools/run_qemu_c-os.sh --bios --cpus 2 --no-network >/dev/null 2>&1 &
for _ in $(seq 1 90); do sleep 2; grep -q "taskbar launched" "$LOG" 2>/dev/null && break; done
sleep 5
ok=0
check() { if grep -aq "$2" "$LOG"; then echo "PASS $1"; else echo "FAIL $1"; ok=1; fi; }
check "a level-2 module (signed with the level-2 key) loads" "\[KMOD\] loaded virtiorng 1.0 level 2"
check "a level-1 module loads" "\[KMOD\] loaded hello 2.0 level 1"
check "the SYSTEM-level write protection is refused: its signing key is not trusted by this kernel" "rejected wprotect.c-osys: bad or missing signature (the key of level 3 is not trusted by this kernel)"
check "... and so is the network filter" "rejected netfw.c-osys: bad or missing signature (the key of level 3 is not trusted by this kernel)"
check "a SYSTEM module signed with the level-1 key is refused here too (the key's level)" "rejected lowkey_system.c-osys"
grep -a "KMOD" "$LOG" | cut -c1-170 | head -20
exit $ok
