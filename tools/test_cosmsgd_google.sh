#!/usr/bin/env bash
# Google sign-in of cosmsgd (host build) against a fake HTTPS Google (tools/fake_google_https.py).
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
tools/cosmsgd_host.sh >/tmp/cosmsgd_hostbuild.log 2>&1 || { tail /tmp/cosmsgd_hostbuild.log; exit 1; }
T=$(mktemp -d); pkill -f fake_google_https.py 2>/dev/null; pkill -x cosmsgd_host 2>/dev/null; sleep 0.3
python3 tools/fake_google_https.py 5171 $T/ca.der test-client.apps.googleusercontent.com test-secret >$T/fake.log 2>&1 & FK=$!
for _ in $(seq 1 50); do grep -q ready $T/fake.log 2>/dev/null && break; sleep 0.2; done
mkdir -p $T/data; cat >$T/data/google.conf <<CONF
client_id=test-client.apps.googleusercontent.com
client_secret=test-secret
require=1
device_host=localhost
token_host=localhost
jwks_host=localhost
port=5171
extra_ca=$T/ca.der
CONF
build/cosmsgd_host 5172 $T/data >$T/srv.log 2>&1 & P=$!; sleep 1
python3 tools/test_cosmsgd_google.py 5172 5171; RC=$?
kill $P $FK 2>/dev/null
[ $RC = 0 ] || { echo "--- server log"; tail -20 $T/srv.log; echo "--- fake log"; tail -5 $T/fake.log; }
exit $RC
