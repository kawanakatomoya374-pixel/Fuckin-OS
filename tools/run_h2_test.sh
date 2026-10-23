#!/usr/bin/env bash
# HTTP/2 regression: builds a VALIDATION_NET=3 image that trusts a throw-away test CA, serves tools/h2_test_server.py
# (TLS + ALPN h2 on the QEMU gateway address) and checks the [NETTEST] lines: flow control, trickling bodies, 1xx, GOAWAY,
# RST_STREAM, stalls, over-size answers, gzip, trailers, fallback to HTTP/1.1.
# Needs: python3 with h2 and cryptography, openssl.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
PORT=18443
D="$ROOT/build/h2test"; mkdir -p "$D"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/ca.key" -out "$D/ca.pem" -days 2 -subj "/CN=cos h2 test CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign" >/dev/null 2>&1 || exit 1
openssl req -newkey rsa:2048 -nodes -keyout "$D/leaf.key" -out "$D/leaf.csr" -subj "/CN=192.168.70.2" >/dev/null 2>&1 || exit 1
openssl x509 -req -in "$D/leaf.csr" -CA "$D/ca.pem" -CAkey "$D/ca.key" -CAcreateserial -out "$D/leaf.pem" -days 2 >/dev/null 2>&1 || exit 1
cat tools/mozilla_ca_bundle.pem "$D/ca.pem" > "$D/bundle.pem"
python3 tools/generate_bearssl_system_trust_store.py --bundle "$D/bundle.pem" --output src/kernel/drivers/generated/bearssl_validation_trust_anchors.inc >/dev/null || exit 1
rm -f obj/kernel/drivers/tls_backend.o
make -j"$(nproc)" iso VALIDATION_NET=3 VALIDATION_SANDBOX_CA=1 >/tmp/h2_build.log 2>&1 || { tail -20 /tmp/h2_build.log; exit 1; }
rm -f build/storage.img; make build/storage.img >/dev/null 2>&1
LOG=$(mktemp /tmp/cos-h2.XXXXXX.log); SRV=$(mktemp /tmp/cos-h2srv.XXXXXX.log)
pkill -x qemu-system-x86 2>/dev/null || true; sleep 2
python3 tools/h2_test_server.py "$PORT" "$D/leaf.pem" "$D/leaf.key" >"$SRV" 2>&1 & SRVPID=$!
trap 'pkill -x qemu-system-x86 2>/dev/null || true; kill $SRVPID 2>/dev/null || true' EXIT
COS_NETDEV="${H2_PCAP:+-netdev user,id=n1,net=192.168.70.0/24,dhcpstart=192.168.70.10,restrict=off -object filter-dump,id=fd,netdev=n1,file=$H2_PCAP}" COS_USB=off COS_VNC="127.0.0.1:$((60 + RANDOM % 30))" COS_SERIAL="$LOG" setsid tools/run_qemu_c-os.sh --bios --cpus 4 >/dev/null 2>&1 &
for _ in $(seq 1 400); do sleep 3; grep -aq "NETTEST\] DONE" "$LOG" 2>/dev/null && break; done
grep -a "NETTEST" "$LOG"
echo "--- server:"; tail -5 "$SRV"
echo "$LOG" > /tmp/h2_last_log; cp "$SRV" /tmp/h2_last_srv
grep -aq "NETTEST\] DONE passed=[0-9]* failed=0" "$LOG"
