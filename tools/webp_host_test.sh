#!/usr/bin/env bash
# WebP decoder regression on the host: builds the ported FFmpeg VP8/VP8L decoder with its host test driver, decodes WebP files
# made with Pillow (lossy, lossless, alpha, odd sizes, 1080p, animated) and compares them with Pillow's own (libwebp) decode.
# Expected: lossless exact, lossy within 1 level (the decoder uses libwebp's BT.601 conversion and "fancy" chroma upsampling),
# animated files show their first frame.   Needs python3 with Pillow.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
W="$ROOT/src/third_party/ffmpeg_webp"
OUT=$(mktemp -d /tmp/webp_test.XXXXXX)
(cd "$W" && gcc -O2 -w -I. -I.. -o "$OUT/webp_host" cos_webp_host_test.c cos_webp_entry.c cos_ffmpeg_shim.c cos_ffmpeg_tables.c webp.c vp8.c vp8dsp.c vp8data.c vpx_rac.c vlc.c videodsp.c h264pred.c libavutil/*.c)
python3 - "$OUT" <<'PY'
import sys, subprocess, math, random
from PIL import Image, ImageDraw, ImageChops, ImageStat
out = sys.argv[1]
random.seed(1)
W, H = 320, 240
im = Image.new("RGB", (W, H)); px = im.load()
for y in range(H):
    for x in range(W):
        px[x, y] = (int(127 + 127 * math.sin(x / 20)), int(127 + 127 * math.sin(y / 17 + x / 50)), (x * y) % 256)
d = ImageDraw.Draw(im); d.ellipse((60, 40, 200, 180), outline=(255, 255, 255), width=5)
for i in range(2000): px[random.randrange(W), random.randrange(H)] = (random.randrange(256),) * 3
a = im.convert("RGBA"); ap = a.load()
for y in range(H):
    for x in range(W): ap[x, y] = ap[x, y][:3] + (int(255 * x / W),)
frames = [im.rotate(i * 20) for i in range(3)]
cases = {
    "lossy90": lambda f: im.save(f, quality=90), "lossy50": lambda f: im.save(f, quality=50),
    "lossless": lambda f: im.save(f, lossless=True), "alpha_lossy": lambda f: a.save(f, quality=85),
    "alpha_lossless": lambda f: a.save(f, lossless=True), "tiny": lambda f: im.resize((1, 1)).save(f),
    "odd": lambda f: im.crop((0, 0, 17, 33)).save(f, quality=80), "big": lambda f: im.resize((1920, 1080)).save(f, quality=80),
    "anim": lambda f: frames[0].save(f, save_all=True, append_images=frames[1:], duration=100, loop=0),
}
bad = 0
for name, make in cases.items():
    f = f"{out}/{name}.webp"; make(f)
    r = subprocess.run([f"{out}/webp_host", f, f"{out}/{name}.ppm"], capture_output=True, text=True)
    if "OK" not in r.stdout: print(f"FAIL {name}: decode failed"); bad += 1; continue
    ref = Image.open(f); ref.seek(0); ref = ref.convert("RGB")
    got = Image.open(f"{out}/{name}.ppm").convert("RGB")
    if ref.size != got.size: print(f"FAIL {name}: size {ref.size} vs {got.size}"); bad += 1; continue
    st = ImageStat.Stat(ImageChops.difference(ref, got)); mean = sum(st.mean) / 3
    limit = 0.0 if "lossless" in name else 0.2
    print(("PASS " if mean <= limit else "FAIL ") + f"{name}: mean abs diff {mean:.3f} (limit {limit})")
    bad += mean > limit
sys.exit(1 if bad else 0)
PY
