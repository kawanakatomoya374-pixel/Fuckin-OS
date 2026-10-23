#!/usr/bin/env bash
# UA_MODE=2 makes the tone start at 44.1 kHz so the resampler is exercised too.
# USB Audio Class + xHCI isochronous OUT regression: builds a VALIDATION_USBAUDIO=1 image (plays a 0.5 s 1 kHz tone as soon
# as the USB audio device is configured), boots it on QEMU with a usb-audio device behind qemu-xhci and records what the
# device received with QEMU's wav audiodev; then checks the recording really contains a ~1 kHz tone at the expected level.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
touch src/kernel/drivers/usb_audio.c
make -j"$(nproc)" iso VALIDATION_USBAUDIO="${UA_MODE:-1}" >/tmp/ua_build.log 2>&1 || { tail -20 /tmp/ua_build.log; exit 1; }
LOG=$(mktemp /tmp/cos-ua.XXXXXX.log); WAV=$(mktemp /tmp/cos-ua.XXXXXX.wav)
trap 'pkill -x qemu-system-x86 2>/dev/null || true; rm -f "$LOG"' EXIT
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
COS_USB=xhci COS_USB_EXTRA="-device usb-audio,bus=xhci.0,audiodev=snd0" COS_AUDIODEV="driver=wav,path=$WAV" \
  COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" \
  setsid tools/run_qemu_c-os.sh --bios --cpus 2 --no-network >/dev/null 2>&1 &
for _ in $(seq 1 60); do sleep 3; grep -q "USBAUDIO\] ready" "$LOG" 2>/dev/null && break; done
sleep 10
pkill -x qemu-system-x86 2>/dev/null || true; sleep 3
FAIL=0
chk() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi; }
chk "USB audio device configured"        "grep -q 'USBAUDIO\] ready: 2ch 16-bit 48000 Hz' '$LOG'"
chk "no kernel fault"                    "! grep -q 'Unhandled kernel page fault' '$LOG'"
python3 - "$WAV" <<'P'
import sys, wave, struct, math
try:
    w = wave.open(sys.argv[1])
except Exception as e:
    print("FAIL  recording unreadable:", e); sys.exit(1)
n = w.getnframes(); ch = w.getnchannels(); rate = w.getframerate()
d = struct.unpack('<%dh' % (n * ch), w.readframes(n))
l = d[0::ch]
# the tone is a square wave of +-8000: find the loud region and measure its frequency by zero crossings
loud = [i for i, v in enumerate(l) if abs(v) > 4000]
if len(loud) < rate // 4:
    print("FAIL  tone too short/absent: %d loud samples of %d (rate %d)" % (len(loud), n, rate)); sys.exit(1)
# the audiodev records silence around the tone: take the first run of loud samples, tolerating gaps < 2 ms
a = loud[0]; b = a
for i in loud[1:]:
    if i - b > rate // 500: break
    b = i
seg = l[a:b + 1]
cross = sum(1 for i in range(1, len(seg)) if (seg[i - 1] < 0) != (seg[i] < 0))
freq = cross / 2 / (len(seg) / rate)
amp = max(abs(v) for v in seg)
print("      recording: %d frames, tone %.3f s, %.0f Hz, peak %d" % (n, len(seg) / rate, freq, amp))
print(("PASS" if 950 < freq < 1050 else "FAIL") + "  tone frequency is 1 kHz")
print(("PASS" if 0.35 < len(seg) / rate < 0.65 else "FAIL") + "  tone length ~0.5 s (no dropped/duplicated packets)")
print(("PASS" if 7000 < amp < 9000 else "FAIL") + "  tone amplitude intact")
sys.exit(0 if (950 < freq < 1050 and 0.35 < len(seg) / rate < 0.65 and 7000 < amp < 9000) else 1)
P
[[ $? -eq 0 ]] || FAIL=1
rm -f "$WAV"
exit $FAIL
