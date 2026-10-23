#!/usr/bin/env bash
# Google sign-in of the C-OS-hosted server: cosmsgd runs inside C-OS (QEMU); the "Google" is tools/fake_google_https.py on the host,
# reached from the guest as 192.168.70.2:5171 (HTTPS, certificate anchored by an extra root injected into the guest's disk).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
T=$(mktemp -d); mkdir -p build
python3 tools/fake_google_https.py 5171 $T/ca.der test-client.apps.googleusercontent.com test-secret >$T/fake.log 2>&1 & FK=$!
for _ in $(seq 1 50); do grep -q ready $T/fake.log 2>/dev/null && break; sleep 0.2; done
cat >$T/google.conf <<CONF
client_id=test-client.apps.googleusercontent.com
client_secret=test-secret
require=1
device_host=192.168.70.2
token_host=192.168.70.2
jwks_host=192.168.70.2
port=5171
extra_ca=/cosmsgd/ca.der
CONF
python3 - $T <<'PY'
import sys
t = sys.argv[1]
arr = lambda n, b: "static const unsigned char %s[%d] = {%s};\n" % (n, len(b), ",".join(str(x) for x in b))
open("build/cosmsgd_test_conf.h", "w").write(arr("TEST_CONF", open(t + "/google.conf", "rb").read()) + arr("TEST_CA", open(t + "/ca.der", "rb").read()))
PY
rm -f obj/kernel/cos_msgsrv.o
make -j"$(nproc)" iso VALIDATION_MSGSRV=2 >/tmp/cosmsgd_build.log 2>&1 || { tail -20 /tmp/cosmsgd_build.log; kill $FK; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-msgsrv.XXXXXX.log); pkill -x qemu-system-x86 2>/dev/null || true; sleep 1
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $FK 2>/dev/null' EXIT
COS_HOSTFWD="tcp:127.0.0.1:15151-:5151" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 120); do sleep 2; grep -aq "COSMSGD\] listening" "$LOG" 2>/dev/null && break; done
python3 tools/test_cosmsgd_google.py 15151 5171 | grep -v '^PASS'; RC=${PIPESTATUS[0]}
grep -a "COSMSGD" "$LOG" | tail -8
exit $RC
