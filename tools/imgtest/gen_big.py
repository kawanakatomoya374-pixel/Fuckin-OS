import sys, os, numpy as np
from PIL import Image
import test_jpeg
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
w, h = 4000, 3000
rng = np.random.default_rng(2)
y, x = np.mgrid[0:h, 0:w].astype(np.float32)
img = np.stack([128 + 100 * np.sin(x / 57 + y / 91), 128 + 100 * np.cos(x / 33 - y / 41), (x + y) * 255 / (w + h)], -1)
img += rng.normal(0, 8, img.shape).astype(np.float32)
im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
im.save(out + '/big.jpg', quality=90, subsampling=2)
im.save(out + '/big_prog.jpg', quality=90, subsampling=2, progressive=True)
im.save(out + '/big.png', compress_level=6)
from testlib import write_png
im.save(out + '/big.bmp')
