import sys, os, io, struct
import numpy as np
from PIL import Image
from testlib import *

def pack_rows(idx_or_px, w, h, bpp, bottom_up=True):
    rows = []
    for y in range(h):
        r = idx_or_px[y]
        if bpp <= 8:
            per = 8 // bpp; b = bytearray()
            for i in range(0, w, per):
                v = 0
                for k in range(per):
                    v |= (int(r[i + k]) if i + k < w else 0) << (8 - bpp * (k + 1))
                b.append(v)
            data = bytes(b)
        elif bpp == 16: data = b''.join(struct.pack('<H', int(v)) for v in r)
        elif bpp == 24: data = bytes(np.asarray(r, np.uint8).reshape(-1))
        else: data = b''.join(struct.pack('<I', int(v)) for v in r)
        data += b'\0' * ((-len(data)) % 4); rows.append(data)
    if bottom_up: rows.reverse()
    return b''.join(rows)

def make_bmp(w, h, bpp, pixels, palette=None, comp=0, masks=None, hdr=40, top_down=False, clrused=None, payload=None, bfsize_wrong=True,
             palette_bytes=None, off_extra=0):
    """pixels: palette indices (h,w) | uint16/uint32 (h,w) | (h,w,3) BGR bytes."""
    core = hdr == 12
    ent = 3 if core else 4
    pal = b''
    if palette is not None:
        pal = b''.join((bytes([b, g, r]) if core else bytes([b, g, r, 0])) for (r, g, b) in palette)
    mask_in_hdr = hdr >= 52 and masks is not None
    h_bytes = b''
    if core: h_bytes = struct.pack('<IHHHH', 12, w, h, 1, bpp)
    else:
        hh = -h if top_down else h
        h_bytes = struct.pack('<IiiHHIIiiII', hdr if hdr != 16 else 40, w, hh, 1, bpp, comp, 0, 2835, 2835, clrused if clrused is not None else (len(palette) if palette is not None else 0), 0)
        if hdr > 40:
            m = masks or (0, 0, 0, 0)
            ext = struct.pack('<IIII', *m) if hdr >= 56 else struct.pack('<III', *m[:3])
            h_bytes += ext + b'\0' * (hdr - 40 - len(ext))
    extra = b''
    if comp in (3, 6) and hdr == 40: extra = struct.pack('<III', *masks[:3]) + (struct.pack('<I', masks[3]) if comp == 6 else b'')
    if payload is None: body = pack_rows(pixels, w, h, bpp, not top_down)
    else: body = payload
    off = 14 + len(h_bytes) + len(extra) + len(pal) + off_extra
    d = b'BM' + struct.pack('<IHHI', 0x12345 if bfsize_wrong else off + len(body), 0, 0, off) + h_bytes + extra + pal + b'\0' * off_extra + body
    return d

def rle_encode(idx, w, h, bpp4, rng, use_abs=True, use_delta=True, holes=None):
    """idx (h,w) in top-down order for a bottom-up file; holes: boolean mask of pixels left undefined"""
    out = bytearray()
    rows = [idx[y] for y in range(h - 1, -1, -1)]; hm = [holes[y] for y in range(h - 1, -1, -1)] if holes is not None else None
    pend_dy = 0
    for y, r in enumerate(rows):
        x = 0
        while x < w:
            if hm is not None and hm[y][x]:
                x0 = x
                while x < w and hm[y][x]: x += 1
                if x < w: out += bytes([0, 2, x - x0 if x - x0 < 256 else 255, 0]); x = x0 + (x - x0 if x - x0 < 256 else 255); 
                continue
            # run of identical
            run = 1
            while x + run < w and run < 255 and r[x + run] == r[x] and not (hm is not None and hm[y][x + run]): run += 1
            if run >= 2 or not use_abs:
                out += bytes([run, (int(r[x]) << 4 | int(r[x + 1 if run > 1 else x])) if bpp4 and run > 1 else (int(r[x]) << 4 | int(r[x]) if bpp4 else int(r[x]))])
                if bpp4 and run > 1:
                    # alternating-nibble run must contain identical pixels so both nibbles are equal
                    out[-1] = (int(r[x]) << 4) | int(r[x])
                x += run; continue
            # absolute run of non-repeating pixels (>=3)
            n = 1
            while x + n < w and n < 100 and not (x + n + 1 < w and r[x + n] == r[x + n + 1]) and not (hm is not None and hm[y][x + n]): n += 1
            if n < 3 or not use_abs:
                for k in range(n):
                    out += bytes([1, (int(r[x + k]) << 4 | int(r[x + k])) if bpp4 else int(r[x + k])])
            else:
                out += bytes([0, n])
                if bpp4:
                    b = bytearray()
                    for k in range(0, n, 2): b.append((int(r[x + k]) << 4) | (int(r[x + k + 1]) if k + 1 < n else 0))
                else: b = bytearray(int(r[x + k]) for k in range(n))
                out += b
                if len(b) & 1: out += b'\0'
            x += n
        out += bytes([0, 0])
    out[-2:] = bytes([0, 1])
    return bytes(out)

def sc(v, m): return (v.astype(np.int64) * 255 // m) if m else np.zeros_like(v)

def run(R):
    rng = np.random.default_rng(5)
    def check(name, d, exp, tol=0, mode='crop', mw=4096, mh=4096):
        p = os.path.join(TMP, 'b.bmp'); open(p, 'wb').write(d)
        got, info = run_dec(p, mode, mw, mh)
        ok = got is not None and got.shape == exp.shape and np.abs(got.astype(int) - exp.astype(int)).max() <= tol
        R.check(name, ok, info[:90])
        return p, got
    for (w, h) in ((1, 1), (5, 3), (17, 9), (32, 4), (33, 7)):
        # palette depths
        for bpp in (1, 2, 4, 8):
            n = 1 << bpp
            for hdr in (40, 12, 108):
                if hdr == 12 and bpp == 2: continue
                pal = rng.integers(0, 256, (n, 3)); idx = rng.integers(0, n, (h, w))
                exp = np.zeros((h, w, 4), np.uint8); exp[..., 3] = 255
                exp[..., :3] = pal[idx]
                for td in ((False, True) if hdr == 40 else (False,)):
                    d = make_bmp(w, h, bpp, idx, pal, hdr=hdr, top_down=td) if hdr != 12 else make_bmp(w, h, bpp, idx, pal, hdr=12)
                    p, got = check('bmp %dbpp pal hdr%d %dx%d td%d' % (bpp, hdr, w, h, td), d, exp)
                    if hdr == 40 and bpp != 2 and got is not None:
                        R.check('bmp %dbpp [oracle==Pillow]' % bpp, (np.array(Image.open(p).convert('RGBA')) == exp).all())
        # clrused smaller than 2^bpp, index beyond palette -> black
        pal = rng.integers(0, 256, (5, 3)); idx = rng.integers(0, 16, (h, w))
        exp = np.zeros((h, w, 4), np.uint8); exp[..., 3] = 255
        full = np.zeros((16, 3), np.int64); full[:5] = pal; exp[..., :3] = full[idx]
        check('bmp 4bpp clrused=5 %dx%d' % (w, h), make_bmp(w, h, 4, idx, pal, clrused=5), exp)
        # 24-bit and 32-bit
        px = rng.integers(0, 256, (h, w, 3)); exp = np.zeros((h, w, 4), np.uint8); exp[..., 3] = 255; exp[..., :3] = px[..., ::-1]
        for hdr in (40, 12, 108, 124):
            for td in (False, True):
                if hdr == 12 and td: continue
                d = make_bmp(w, h, 24, px, hdr=hdr, top_down=td)
                p, got = check('bmp 24bpp hdr%d %dx%d td%d' % (hdr, w, h, td), d, exp)
                if hdr == 40 and got is not None: R.check('bmp 24 [oracle==Pillow]', (np.array(Image.open(p).convert('RGBA')) == exp).all())
        px32 = rng.integers(0, 2 ** 32, (h, w), dtype=np.uint64).astype(np.uint32)
        a = (px32 >> 24).astype(np.uint8)
        e_rgb = np.zeros((h, w, 4), np.uint8); e_rgb[..., 0] = (px32 >> 16) & 255; e_rgb[..., 1] = (px32 >> 8) & 255; e_rgb[..., 2] = px32 & 255
        e = e_rgb.copy(); e[..., 3] = np.where(a > 0, a, 255); e[0, 0, 3] = 77; px32[0, 0] = (px32[0, 0] & 0x00FFFFFF) | (77 << 24); a = (px32 >> 24).astype(np.uint8)
        check('bmp 32bpp BI_RGB alpha used %dx%d' % (w, h), make_bmp(w, h, 32, px32), e)
        px0 = px32 & 0x00FFFFFF; e0 = e_rgb.copy(); e0[..., 0] = (px0 >> 16) & 255; e0[..., 1] = (px0 >> 8) & 255; e0[..., 2] = px0 & 255; e0[..., 3] = 255
        check('bmp 32bpp BI_RGB all-zero alpha -> opaque %dx%d' % (w, h), make_bmp(w, h, 32, px0), e0)
        for comp, hdr, nm in ((3, 40, 'BITFIELDS rgb hdr40'), (6, 40, 'ALPHABITFIELDS hdr40'), (3, 108, 'BITFIELDS hdr108'), (3, 56, 'BITFIELDS hdr56')):
            masks = (0xFF0000, 0xFF00, 0xFF, 0xFF000000 if comp == 6 or hdr >= 56 else 0)
            ee = e_rgb.copy(); ee[..., 3] = a if masks[3] else 255
            check('bmp 32bpp %s %dx%d' % (nm, w, h), make_bmp(w, h, 32, px32, comp=comp, masks=masks, hdr=hdr), ee)
        # 10-10-10 masks
        m10 = (0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000)
        v = px32.astype(np.uint64)
        e10 = np.zeros((h, w, 4), np.uint8)
        e10[..., 0] = (((v & m10[0]) >> 20) * 255 // 1023); e10[..., 1] = (((v & m10[1]) >> 10) * 255 // 1023); e10[..., 2] = ((v & m10[2]) * 255 // 1023); e10[..., 3] = (((v & m10[3]) >> 30) * 255 // 3)
        check('bmp 32bpp 2:10:10:10 %dx%d' % (w, h), make_bmp(w, h, 32, px32, comp=6, masks=m10, hdr=108), e10)
        # 16-bit: 555 default, 565, 4444
        p16 = rng.integers(0, 65536, (h, w)).astype(np.uint16); v = p16.astype(np.int64)
        e = np.zeros((h, w, 4), np.uint8); e[..., 3] = 255
        e[..., 0] = ((v >> 10) & 31) * 255 // 31; e[..., 1] = ((v >> 5) & 31) * 255 // 31; e[..., 2] = (v & 31) * 255 // 31
        check('bmp 16bpp 555 %dx%d' % (w, h), make_bmp(w, h, 16, p16), e)
        e[..., 0] = ((v >> 11) & 31) * 255 // 31; e[..., 1] = ((v >> 5) & 63) * 255 // 63; e[..., 2] = (v & 31) * 255 // 31
        p, got = check('bmp 16bpp 565 bitfields %dx%d' % (w, h), make_bmp(w, h, 16, p16, comp=3, masks=(0xF800, 0x07E0, 0x001F, 0)), e)
        e4 = np.zeros((h, w, 4), np.uint8)
        for i, (m, s) in enumerate(((0x0F00, 8), (0x00F0, 4), (0x000F, 0), (0xF000, 12))): e4[..., i] = ((v & m) >> s) * 255 // 15
        check('bmp 16bpp 4444 alphabitfields %dx%d' % (w, h), make_bmp(w, h, 16, p16, comp=6, masks=(0x0F00, 0x00F0, 0x000F, 0xF000)), e4)
        # RLE8 / RLE4 (full coverage, with runs/absolute mode) and with holes (transparent)
        for bpp4 in (False, True):
            n = 16 if bpp4 else 256; pal = rng.integers(0, 256, (n, 3))
            idx = rng.integers(0, 3 if rng.integers(0, 2) else n, (h, w))
            for holes_on in (False, True):
                holes = (rng.random((h, w)) < 0.3) if holes_on else None
                exp = np.zeros((h, w, 4), np.uint8); exp[..., 3] = 255; exp[..., :3] = pal[idx]
                if holes is not None: exp[holes] = 0
                body = rle_encode(idx, w, h, bpp4, rng, holes=holes)
                d = make_bmp(w, h, 4 if bpp4 else 8, None, pal, comp=2 if bpp4 else 1, payload=body)
                p, got = check('bmp RLE%d %dx%d holes%d' % (4 if bpp4 else 8, w, h, holes_on), d, exp)
                if holes is None and got is not None:
                    try: pil_ok = (np.array(Image.open(p).convert('RGBA')) == exp).all()
                    except Exception as e2: pil_ok = None
                    if not pil_ok: R.note('RLE cross-check: Pillow %s on a stream our decoder reads as intended' % ('rejects' if pil_ok is None else 'differs'))
    # RLE: delta escape and early EOB (rest transparent), top-down RLE
    pal = [(255, 0, 0), (0, 255, 0), (0, 0, 255)] + [(0, 0, 0)] * 253
    body = bytes([3, 1, 0, 2, 2, 1, 2, 2, 0, 1])      # 3x idx1; delta(+2,+1) -> x=5,y=1; 2x idx2; EOB
    d = make_bmp(8, 4, 8, None, pal, comp=1, payload=body)
    exp = np.zeros((4, 8, 4), np.uint8); exp[3, 0:3] = [0, 255, 0, 255]; exp[2, 5:7] = [0, 0, 255, 255]
    check('bmp RLE8 delta + EOB', d, exp)
    # embedded PNG / JPEG
    arr = rng.integers(0, 256, (6, 9, 4)); pb = os.path.join(TMP, 'e.png'); write_png(pb, arr, 6, 8)
    d = make_bmp(9, 6, 0, None, comp=5, payload=open(pb, 'rb').read()); check('bmp BI_PNG', d, arr.astype(np.uint8))
    jb = io.BytesIO(); Image.fromarray(arr[..., :3].astype(np.uint8)).save(jb, 'JPEG', quality=90); jp = jb.getvalue()
    ref = np.array(Image.open(io.BytesIO(jp)).convert('RGB')); e = np.dstack([ref, np.full(ref.shape[:2], 255, np.uint8)])
    check('bmp BI_JPEG', make_bmp(9, 6, 0, None, comp=4, payload=jp), e)
    # pixel data offset: padding between palette and pixels (gap), bad offsets
    pal = rng.integers(0, 256, (256, 3)); idx = rng.integers(0, 256, (4, 6)); exp = np.zeros((4, 6, 4), np.uint8); exp[..., 3] = 255; exp[..., :3] = pal[idx]
    check('bmp gap before pixel data (bfOffBits)', make_bmp(6, 4, 8, idx, pal, off_extra=10), exp)
    d = bytearray(make_bmp(6, 4, 8, idx, pal)); d[10:14] = struct.pack('<I', 20); got = os.path.join(TMP, 'b.bmp'); open(got, 'wb').write(bytes(d))
    R.check('bmp reject offset inside header', run_dec(got)[0] is None)
    d[10:14] = struct.pack('<I', 0x7FFFFFFF); open(got, 'wb').write(bytes(d)); R.check('bmp reject offset beyond EOF', run_dec(got)[0] is None)
    d = make_bmp(6, 4, 8, idx, pal); open(got, 'wb').write(d[:-3]); R.check('bmp reject truncated pixel data', run_dec(got)[0] is None)
    for nm, off, val in (('planes', 26, 2), ('bpp', 28, 7)):
        d = bytearray(make_bmp(6, 4, 8, idx, pal)); d[off:off + 2] = struct.pack('<H', val); open(got, 'wb').write(bytes(d)); R.check('bmp reject bad ' + nm, run_dec(got)[0] is None)
    d = bytearray(make_bmp(6, 4, 8, idx, pal)); d[18:22] = struct.pack('<i', 0); open(got, 'wb').write(bytes(d)); R.check('bmp reject zero width', run_dec(got)[0] is None)
    d[18:22] = struct.pack('<i', -5); open(got, 'wb').write(bytes(d)); R.check('bmp reject negative width', run_dec(got)[0] is None)
    # crop / fit
    px = rng.integers(0, 256, (60, 90, 3)); exp = np.zeros((60, 90, 4), np.uint8); exp[..., 3] = 255; exp[..., :3] = px[..., ::-1]
    d = make_bmp(90, 60, 24, px); p = os.path.join(TMP, 'bf.bmp'); open(p, 'wb').write(d)
    got, info = run_dec(p, 'crop', 40, 20); R.check('bmp crop', got is not None and (got == exp[:20, :40]).all())
    from test_png import area_ref
    for (mw, mh) in ((45, 45), (90, 10), (7, 60), (89, 59)):
        got, info = run_dec(p, 'fit', mw, mh)
        ok = got is not None and got.shape[0] <= mh and got.shape[1] <= mw
        if ok: ok = np.abs(got.astype(int) - area_ref(exp, got.shape[1], got.shape[0]).astype(int)).max() <= 1
        R.check('bmp fit %dx%d' % (mw, mh), ok, info[:80])

if __name__ == '__main__':
    R = Results(); run(R)
    print('bmp: %d passed, %d failed' % (R.passed, len(R.failed)))
    sys.exit(1 if R.failed else 0)
