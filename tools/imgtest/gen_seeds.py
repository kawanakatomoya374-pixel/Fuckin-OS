"""Writes small seed files for fuzz.c into the directory given as argv[1]."""
import sys, os, io, gzip, struct
import numpy as np
from PIL import Image
from testlib import write_png
import test_bmp, test_jpeg

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
rng = np.random.default_rng(1)
def put(n, d): open(os.path.join(out, n), 'wb').write(d)
im = test_jpeg.photo(41, 29)
for nm, kw in (('base', {}), ('prog', dict(progressive=True)), ('rst', dict(restart_marker_blocks=2)), ('444', dict(subsampling=0))):
    b = io.BytesIO(); im.save(b, 'JPEG', quality=70, **kw); put('s_%s.jpg' % nm, b.getvalue())
ex = Image.Exif(); ex[0x112] = 6; b = io.BytesIO(); im.save(b, 'JPEG', exif=ex.tobytes(), subsampling=2); put('s_exif.jpg', b.getvalue())
b = io.BytesIO(); im.convert('CMYK').save(b, 'JPEG'); put('s_cmyk.jpg', b.getvalue())
b = io.BytesIO(); im.convert('L').save(b, 'JPEG', progressive=True); put('s_gray_prog.jpg', b.getvalue())
import pyjpeg
pl = pyjpeg.ycc(np.array(im)); q = pyjpeg.std_q(.6)
put('s_multiscan.jpg', pyjpeg.encode(pl, 41, 29, [(2, 2), (1, 1), (1, 1)], [q, q], interleaved=False, restart=3)[0])
put('s_440.jpg', pyjpeg.encode(pl, 41, 29, [(1, 2), (1, 1), (1, 1)], [q, q], sof=0xC1)[0])
for ct, d in ((0, 8), (0, 1), (2, 16), (3, 4), (4, 8), (6, 8), (6, 16)):
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    arr = rng.integers(0, 1 << d if ct != 3 else 16, (17, 23, ch))
    pal = rng.integers(0, 256, (16, 3)) if ct == 3 else None
    trns = b'\x00\x01' if ct == 0 and d == 1 else (b'\x10\x20\x30' if ct == 3 else None)
    for il in (False, True): write_png(os.path.join(out, 's_ct%dd%d_%d.png' % (ct, d, il)), arr, ct, d, pal, trns, il, idat_split=40)
pal = rng.integers(0, 256, (256, 3)); idx = rng.integers(0, 256, (9, 11))
put('s_pal8.bmp', test_bmp.make_bmp(11, 9, 8, idx, pal))
put('s_rgb24.bmp', test_bmp.make_bmp(11, 9, 24, rng.integers(0, 256, (9, 11, 3))))
put('s_bf16.bmp', test_bmp.make_bmp(11, 9, 16, rng.integers(0, 65536, (9, 11)).astype(np.uint16), comp=3, masks=(0xF800, 0x7E0, 0x1F, 0)))
put('s_rgb32.bmp', test_bmp.make_bmp(11, 9, 32, rng.integers(0, 2 ** 32, (9, 11), dtype=np.uint64).astype(np.uint32), comp=6, masks=(0xFF0000, 0xFF00, 0xFF, 0xFF000000), hdr=108))
pal16 = rng.integers(0, 256, (16, 3)); i4 = rng.integers(0, 16, (9, 11))
put('s_rle4.bmp', test_bmp.make_bmp(11, 9, 4, None, pal16, comp=2, payload=test_bmp.rle_encode(i4, 11, 9, True, rng)))
put('s_rle8.bmp', test_bmp.make_bmp(11, 9, 8, None, pal, comp=1, payload=test_bmp.rle_encode(idx % 5, 11, 9, False, rng)))
put('s_core.bmp', test_bmp.make_bmp(11, 9, 4, i4, pal16, hdr=12))
put('s.gz', gzip.compress(bytes(rng.integers(0, 6, 5000).astype(np.uint8)) + b'text ' * 300))
