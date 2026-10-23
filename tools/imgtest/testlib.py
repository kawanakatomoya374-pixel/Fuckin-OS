"""Shared helpers for the image decoder tests (Pillow/numpy are the oracle)."""
import os, subprocess, struct, zlib
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
IMGDEC = os.path.join(os.environ.get('IMGTEST_BUILD', os.path.join(HERE, 'build')), 'imgdec')
TMP = os.environ.get('IMGTEST_TMP', '/tmp/imgtest')
os.makedirs(TMP, exist_ok=True)

class Results:
    def __init__(self): self.passed = 0; self.failed = []; self.notes = []
    def check(self, name, ok, detail=''):
        if ok: self.passed += 1
        else:
            self.failed.append((name, detail)); print('FAIL', name, detail)
    def note(self, s): self.notes.append(s)

def run_dec(path, mode='crop', mw=4096, mh=4096, flags=0):
    """-> (rgba ndarray (h,w,4) RGBA order | None, info string)"""
    raw = os.path.join(TMP, 'out.raw')
    if os.path.exists(raw): os.remove(raw)
    r = subprocess.run([IMGDEC, mode, path, raw, str(mw), str(mh), str(flags)], capture_output=True, text=True)
    out = r.stdout.strip() + r.stderr.strip()[:2000]
    if r.returncode != 0 or not out.startswith('OK'):
        return None, out
    t = out.split()
    w, h = int(t[1]), int(t[2])
    a = np.fromfile(raw, dtype=np.uint8).reshape(h, w, 4)
    return a[:, :, [2, 1, 0, 3]], out

def run_gunzip(path, cap):
    outp = os.path.join(TMP, 'gz.out')
    r = subprocess.run([IMGDEC, 'gunzip', path, outp, str(cap)], capture_output=True, text=True)
    if not r.stdout.startswith('OK'): return None
    return open(outp, 'rb').read()

# ---------------------------------------------------------------- PNG writer
def _chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)

A7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]

def _pack_row(samples, depth):
    """samples: 1-D int array of per-channel samples for one row -> bytes"""
    if depth == 8: return bytes(samples.astype(np.uint8))
    if depth == 16: return b''.join(struct.pack('>H', int(v)) for v in samples)
    per = 8 // depth; out = bytearray(); n = len(samples)
    for i in range(0, n, per):
        b = 0
        for k in range(per):
            v = int(samples[i + k]) if i + k < n else 0
            b |= v << (8 - depth * (k + 1))
        out.append(b)
    return bytes(out)

def _filter_rows(rows, bpp, rng, filters):
    out = bytearray(); prev = bytes(len(rows[0])) if rows else b''
    for r in rows:
        f = int(rng.choice(filters)); cur = np.frombuffer(r, np.uint8).astype(np.int32); pv = np.frombuffer(prev, np.uint8).astype(np.int32)
        n = len(cur); res = np.zeros(n, np.int32)
        for i in range(n):
            a = cur[i - bpp] if i >= bpp else 0
            b = pv[i]; c = pv[i - bpp] if i >= bpp else 0
            if f == 0: p = 0
            elif f == 1: p = a
            elif f == 2: p = b
            elif f == 3: p = (a + b) // 2
            else:
                pp = a + b - c; pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                p = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            res[i] = (cur[i] - p) & 255
        out.append(f); out += bytes(res.astype(np.uint8)); prev = r
    return bytes(out)

def write_png(path, arr, ctype, depth, palette=None, trns=None, interlace=False, seed=0, filters=(0, 1, 2, 3, 4),
              idat_split=0, extra_chunks=(), apng=False):
    """arr: (h,w,channels) ints in sample range. trns: bytes for the tRNS chunk."""
    rng = np.random.default_rng(seed)
    h, w, ch = arr.shape
    bpp = max(1, ch * depth // 8)
    def encode(sub):
        hh, ww, _ = sub.shape
        if hh == 0 or ww == 0: return b''
        rows = [_pack_row(sub[y].reshape(-1), depth) for y in range(hh)]
        return _filter_rows(rows, bpp, rng, filters)
    if interlace:
        raw = b''.join(encode(arr[y0::dy, x0::dx]) for (x0, y0, dx, dy) in A7)
    else:
        raw = encode(arr)
    z = zlib.compress(raw, 6)
    out = b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, 1 if interlace else 0))
    for c in extra_chunks: out += c
    if apng: out += _chunk(b'acTL', struct.pack('>II', 2, 0)) + _chunk(b'fcTL', b'\0' * 26)
    if palette is not None: out += _chunk(b'PLTE', bytes(np.asarray(palette, np.uint8).reshape(-1)))
    if trns is not None: out += _chunk(b'tRNS', trns)
    if idat_split:
        for i in range(0, len(z), idat_split): out += _chunk(b'IDAT', z[i:i + idat_split])
    else: out += _chunk(b'IDAT', z)
    if apng: out += _chunk(b'fcTL', b'\0' * 26) + _chunk(b'fdAT', b'\0\0\0\1' + zlib.compress(b'\0' * 10))
    out += _chunk(b'IEND', b'')
    open(path, 'wb').write(out)
    return out

def scale16(v): return (v.astype(np.int64) * 255 + 32767) // 65535

def png_expected(arr, ctype, depth, palette=None, trns=None):
    """Independent reference conversion to RGBA8 (python/numpy)."""
    h, w, ch = arr.shape
    a = arr.astype(np.int64)
    out = np.zeros((h, w, 4), np.uint8); out[..., 3] = 255
    maxv = (1 << depth) - 1
    def sc(v):
        return scale16(v) if depth == 16 else (v * 255 // maxv)
    if ctype == 0:
        g = sc(a[..., 0]); out[..., 0] = out[..., 1] = out[..., 2] = g
        if trns is not None:
            key = struct.unpack('>H', trns)[0] & maxv if depth < 16 else struct.unpack('>H', trns)[0]
            out[..., 3] = np.where(a[..., 0] == key, 0, 255)
    elif ctype == 2:
        for i in range(3): out[..., i] = sc(a[..., i])
        if trns is not None:
            k = struct.unpack('>HHH', trns); k = [x & maxv for x in k] if depth < 16 else list(k)
            m = (a[..., 0] == k[0]) & (a[..., 1] == k[1]) & (a[..., 2] == k[2])
            out[..., 3] = np.where(m, 0, 255)
    elif ctype == 3:
        pal = np.zeros((256, 4), np.uint8); pal[:, 3] = 255
        p = np.asarray(palette, np.uint8).reshape(-1, 3); pal[:len(p), :3] = p
        if trns is not None:
            for i, t in enumerate(trns[:256]): pal[i, 3] = t
        out = pal[a[..., 0]]
    elif ctype == 4:
        g = sc(a[..., 0]); out[..., 0] = out[..., 1] = out[..., 2] = g; out[..., 3] = sc(a[..., 1])
    elif ctype == 6:
        for i in range(4): out[..., i] = sc(a[..., i])
    return out
