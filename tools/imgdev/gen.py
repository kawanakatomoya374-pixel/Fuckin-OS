#!/usr/bin/env python3
"""Generate the images of the on-device decoder test (tools/run_imgdev_test.sh) and manifest.json
(expected output size and a 4x4 grid of average colours per file, from Pillow)."""
import json, os, struct, sys, zlib
import numpy as np
from PIL import Image, ImageOps

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
FIT = (1920, 1080)
man = {}

def scene(w, h, smooth=False):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    if smooth:      # compresses well (the big PNG goes into the kernel image)
        r = x / w * 255; g = y / h * 255; b = (x + y) / (w + h) * 255
    else:
        r = 128 + 110 * np.sin(x / (w / 9.0) + y / (h / 7.0))
        g = 128 + 110 * np.cos(x / (w / 5.0) - y / (h / 11.0))
        b = (x / w * 200 + y / h * 55)
    a = np.stack([r, g, b], -1)
    # four flat blocks so orientation errors are visible
    a[: h // 4, : w // 4] = (250, 20, 20)
    a[: h // 4, -w // 4:] = (20, 250, 20)
    a[-h // 4:, : w // 4] = (20, 20, 250)
    a[-h // 4:, -w // 4:] = (250, 250, 20)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

def grid(im, n=4):
    a = np.asarray(im.convert('RGB'), dtype=np.float64)
    h, w = a.shape[:2]
    cells = []
    for gy in range(n):
        for gx in range(n):
            blk = a[gy * h // n:(gy + 1) * h // n, gx * w // n:(gx + 1) * w // n]
            cells.append([int(round(v)) for v in blk.reshape(-1, 3).mean(0)])
    return cells

def expect(name, im, note=''):
    w, h = im.size
    if name.endswith('.jpg'):
        # JPEG is shrunk in the DCT domain first: the smallest divisor of 1, 2, 4, 8 that fits, then area-averaged if 1/8 is still too big
        d = next((d for d in (1, 2, 4, 8) if -(-w // d) <= FIT[0] and -(-h // d) <= FIT[1]), 8)
        w2, h2 = -(-w // d), -(-h // d)
        s = min(1.0, FIT[0] / w2, FIT[1] / h2)
        ew, eh = max(1, int(round(w2 * s))), max(1, int(round(h2 * s)))
    else:
        s = min(1.0, FIT[0] / w, FIT[1] / h)
        ew, eh = max(1, int(round(w * s))), max(1, int(round(h * s)))
    ref = im.resize((ew, eh), Image.BOX) if (ew, eh) != (w, h) else im
    man[name] = {'w': ew, 'h': eh, 'grid': grid(ref), 'note': note}

def bad(name, note):
    man[name] = {'w': 0, 'h': 0, 'grid': None, 'note': note}   # must not crash; a failure or a placeholder is fine

big = scene(4000, 3000)
big.save(f'{out}/big_base.jpg', quality=85, subsampling=2);  expect('big_base.jpg', big, 'baseline 4:2:0, 12 Mpixel')
big.save(f'{out}/big_prog.jpg', quality=85, subsampling=1, progressive=True); expect('big_prog.jpg', big, 'progressive 4:2:2')
bigs = scene(4000, 3000, smooth=True)
bigs.save(f'{out}/big.png', optimize=True); expect('big.png', bigs, 'PNG 12 Mpixel')
mid = scene(640, 480, smooth=True)
mid.save(f'{out}/mid24.bmp'); expect('mid24.bmp', mid, '24-bit BMP (no scaling)')
mid.convert('RGBA').save(f'{out}/mid32.bmp'); expect('mid32.bmp', mid, '32-bit BMP')
mid.convert('P', palette=Image.ADAPTIVE, colors=64).save(f'{out}/mid_pal_adam7.png', interlace=1) if False else None
small = scene(640, 480)
small.save(f'{out}/small.jpg', quality=92, subsampling=0); expect('small.jpg', small, '4:4:4 no scaling')
small.convert('L').save(f'{out}/gray.jpg', quality=90); expect('gray.jpg', small.convert('L').convert('RGB'), 'grayscale JPEG')
small.convert('CMYK').save(f'{out}/cmyk.jpg', quality=90); expect('cmyk.jpg', small, 'CMYK JPEG (inverted by Pillow when writing)')
# EXIF orientation 6: the stored picture is rotated, the viewer must turn it upright (the output is 480x640)
ex = Image.Exif(); ex[0x0112] = 6
small.save(f'{out}/exif6.jpg', quality=92, subsampling=0, exif=ex.tobytes())
up = ImageOps.exif_transpose(Image.open(f'{out}/exif6.jpg')); expect('exif6.jpg', up, 'EXIF orientation 6')
# palette PNG, interlaced
pal = scene(900, 700).convert('P', palette=Image.ADAPTIVE, colors=32)
pal.save(f'{out}/pal_adam7.png', interlace=1) if False else pal.save(f'{out}/pal.png'); expect('pal.png', pal.convert('RGB'), '8-bit palette PNG')
scene(500, 400).convert('RGBA').save(f'{out}/rgba.png'); expect('rgba.png', scene(500, 400), 'RGBA PNG')
scene(500, 400).convert('L').save(f'{out}/gray16.png'); expect('gray16.png', scene(500, 400).convert('L').convert('RGB'), 'gray PNG')
# 4000x3000 RLE8 BMP of horizontal bands (a few KB, 12 Mpixel)
def rle8(w, h):
    pal = bytearray()
    for i in range(256):
        pal += bytes((i, 255 - i, (i * 7) & 255, 0))   # B G R 0
    rows = []
    for y in range(h):
        idx = (y * 255) // h
        row = bytearray()
        left = w
        while left:
            n = min(255, left); row += bytes((n, idx)); left -= n
        row += b'\x00\x00'
        rows.append(bytes(row))
    data = b''.join(reversed(rows)) + b'\x00\x01'
    off = 14 + 40 + len(pal)
    hdr = struct.pack('<2sIHHI', b'BM', off + len(data), 0, 0, off)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 8, 1, len(data), 2835, 2835, 256, 0)
    return hdr + info + bytes(pal) + data
open(f'{out}/big_rle8.bmp', 'wb').write(rle8(4000, 3000))
ref = np.zeros((3000, 4000, 3), np.uint8)
for y in range(3000):
    i = (y * 255) // 3000; ref[y, :] = (i * 7 & 255, 255 - i, i)
expect('big_rle8.bmp', Image.fromarray(ref), 'RLE8 BMP 12 Mpixel')
# damaged input: none of these may crash or hang the system
j = open(f'{out}/small.jpg', 'rb').read(); open(f'{out}/trunc.jpg', 'wb').write(j[: len(j) * 6 // 10]); bad('trunc.jpg', 'JPEG cut at 60%')
p = bytearray(open(f'{out}/rgba.png', 'rb').read()); p[len(p) // 2] ^= 0xFF; open(f'{out}/badcrc.png', 'wb').write(bytes(p)); bad('badcrc.png', 'PNG with a flipped byte')
def png_chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
bomb = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 60000, 60000, 8, 6, 0, 0, 0)) + png_chunk(b'IDAT', zlib.compress(b'\0' * 100)) + png_chunk(b'IEND', b'')
open(f'{out}/bomb.png', 'wb').write(bomb); bad('bomb.png', 'PNG header claims 60000x60000')
open(f'{out}/empty.png', 'wb').write(b''); bad('empty.png', 'zero-length file')
open(f'{out}/garbage.jpg', 'wb').write(os.urandom(5000)); bad('garbage.jpg', 'random bytes')
open(f'{out}/notimg.bmp', 'wb').write(b'BM' + os.urandom(300)); bad('notimg.bmp', 'BM + random bytes')
json.dump(man, open(f'{out}/manifest.json', 'w'), indent=1)
print('%d files, %d KiB' % (len(man), sum(os.path.getsize(f'{out}/{n}') for n in man) // 1024))
