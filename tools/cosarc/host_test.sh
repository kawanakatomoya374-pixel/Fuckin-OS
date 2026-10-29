#!/usr/bin/env bash
# Builds userland/lib/cosarc*.c for the host (-DCOSARC_HOST) and checks it against the system gzip, tar and unzip, both directions.
# usage: tools/cosarc/host_test.sh        (needs gzip, tar, unzip, python3)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
gcc -O2 -DCOSARC_HOST -D_FILE_OFFSET_BITS=64 -I"$ROOT/userland/lib" -o "$W/arc" "$ROOT/tools/cosarc/host_main.c" "$ROOT/userland/lib/cosarc.c" "$ROOT/userland/lib/cosarc_cli.c"
cd "$W"; mkdir -p src/sub/deep; echo hello > src/a.txt; head -c 200000 /dev/urandom > src/rnd.bin; cat /usr/include/*.h | head -c 3000000 > src/text.bin; echo x > src/sub/deep/inner.txt
L=$(python3 -c "print('d'*60+'/'+'e'*60+'/'+'f'*60)"); mkdir -p "src/$L"; echo long > "src/$L/$(python3 -c "print('g'*90)").txt"
A="$W/arc"
cp src/text.bin g.bin; $A gzip -k g.bin >/dev/null; gzip -dc g.bin.gz | cmp - src/text.bin; gzip -n -c src/rnd.bin > r.gz; $A gunzip r.gz >/dev/null; cmp r src/rnd.bin
$A tar -czf t.tgz src >/dev/null; mkdir x; tar -xzf t.tgz -C x; diff -r src x/src
tar -czf s.tgz src; mkdir y; $A tar -xf s.tgz -C y >/dev/null; diff -r src y/src
tar --format=gnu -cf g.tar src; mkdir y2; $A tar -xf g.tar -C y2 >/dev/null; diff -r src y2/src
$A zip z.zip src >/dev/null; unzip -tq z.zip >/dev/null; mkdir z; unzip -q z.zip -d z; diff -r src z/src
python3 -c "
import zipfile,os
z=zipfile.ZipFile('p.zip','w',zipfile.ZIP_DEFLATED)
for r,d,f in os.walk('src'):
    for n in f: z.write(os.path.join(r,n))
z.close()"; mkdir p; $A unzip p.zip -d p >/dev/null; diff -r src p/src
echo "cosarc host test: all OK"
