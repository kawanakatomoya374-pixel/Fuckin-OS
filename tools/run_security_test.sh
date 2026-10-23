#!/usr/bin/env bash
# Process-security regression: boots a VALIDATION_SANDBOX=1 image, which runs tests/sandbox_test three times, and checks
#  - SMEP is switched on for every CPU,
#  - the capability sandbox and the file-system jail behave (all [SANDBOX] checks pass, in every instance),
#  - ASLR: the three processes got different stack and heap addresses.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
CPUS=4
make -j"$(nproc)" iso VALIDATION_SANDBOX=1 >/tmp/sec_build.log 2>&1 || { tail -20 /tmp/sec_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-sec.XXXXXX.log)
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
trap 'pkill -x qemu-system-x86 2>/dev/null || true; rm -f "$LOG"' EXIT
COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus $CPUS --no-network >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; [[ $(grep -ac "SANDBOX\] DONE" "$LOG") -ge 3 ]] && break; done
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
chk "SMEP enabled on every CPU"             "[[ \$(grep -ac 'SMEP enabled' '$LOG') -ge $CPUS ]]"
chk "all three sandbox instances finished"  "[[ \$(grep -ac 'SANDBOX\] DONE failed=0' '$LOG') -eq 3 ]]"
chk "no sandbox check failed"               "! grep -aq 'SANDBOX\] FAIL' '$LOG'"
chk "the kernel denied the dropped syscalls" "grep -aq 'SANDBOX\] syscall .* denied' '$LOG'"
chk "jail rejected out-of-jail paths"       "grep -aq 'outside the process.s jail' '$LOG'"
STACKS=$(grep -a "SANDBOX\] layout" "$LOG" | sed -n 's/.*stack=\(0x[0-9a-f]*\).*/\1/p' | sort -u | wc -l)
HEAPS=$(grep -a "SANDBOX\] layout" "$LOG" | sed -n 's/.*heap=\(0x[0-9a-f]*\).*/\1/p' | sort -u | wc -l)
chk "ASLR: three different stack addresses" "[[ $STACKS -eq 3 ]]"
CODES=$(grep -a "SANDBOX\] layout" "$LOG" | sed -n 's/.*code=\(0x[0-9a-f]*\).*/\1/p' | sort -u | wc -l)
chk "ASLR: three different heap addresses"  "[[ $HEAPS -eq 3 ]]"
chk "ASLR: three different executable load addresses" "[[ $CODES -eq 3 ]]"
chk "SMAP enabled on every CPU"             "[[ \$(grep -ac 'SMAP enabled' '$LOG') -ge $CPUS ]]"
chk "no SMAP violation"                     "! grep -aq '\\[SMAP\\]' '$LOG'"
chk "no kernel fault"                       "! grep -aq 'Unhandled kernel page fault\|stack smashing' '$LOG'"
grep -a "SANDBOX\] \(PASS\|FAIL\|layout\)" "$LOG" | head -20
exit $FAIL
