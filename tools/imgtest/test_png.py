import sys, os, zlib, struct, gzip, io
import numpy as np
from PIL import Image
from testlib import *

def run(R):
    rng = np.random.default_rng(7)
    combos = [(0, d) for d in (1, 2, 4, 8, 16)] + [(2, 8), (2, 16)] + [(3, d) for d in (1, 2, 4, 8)] + [(4, 8), (4, 16), (6, 8), (6, 16)]
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    n = 0
    for ct, d in combos:
        for (w, h) in ((1, 1), (7, 5), (13, 11), (33, 20)):
            for il in (False, True):
                ch = chans[ct]; mx = (1 << d) - 1
                if ct == 3:
                    npal = min(1 << d, 256); arr = rng.integers(0, npal, (h, w, 1))
                    pal = rng.integers(0, 256, (npal, 3)); trns = bytes(rng.integers(0, 256, rng.integers(1, npal + 1)).astype(np.uint8)) if n % 2 else None
                else:
                    arr = rng.integers(0, mx + 1, (h, w, ch)); pal = None; trns = None
                    if ct in (0, 2) and n % 3 == 0:                       # colour-key transparency, make sure some pixels match
                        key = [int(arr[0, 0, i]) for i in range(ch)]
                        for _ in range(3): arr[rng.integers(0, h), rng.integers(0, w)] = key
                        trns = b''.join(struct.pack('>H', k) for k in key)
                p = os.path.join(TMP, 'p.png')
                write_png(p, arr, ct, d, pal, trns, il, seed=n, idat_split=7 if n % 4 == 0 else 0)
                exp = png_expected(arr, ct, d, pal, trns)
                got, info = run_dec(p)
                name = 'png ct%d d%d %dx%d il%d trns%d' % (ct, d, w, h, il, trns is not None)
                R.check(name, got is not None and got.shape == exp.shape and (got == exp).all(), info[:80])
                # cross-check the oracle against Pillow where Pillow is faithful (<=8 bit)
                if d <= 8 and got is not None and not (ct == 0 and d < 8 and trns is not None):   # Pillow compares the colour key after scaling (its quirk)
                    pil = np.array(Image.open(p).convert('RGBA'))
                    if ct in (0, 2, 3) and trns is not None and ct != 3:
                        pil_ok = (pil[..., :3] == exp[..., :3]).all() and (pil[..., 3] == exp[..., 3]).all()
                    else:
                        pil_ok = (pil == exp).all() if ct != 3 or trns is None else (pil == exp).all()
                    R.check(name + ' [oracle==Pillow]', pil_ok)
                n += 1
    # Pillow-written files (real-world encoder: optimize, all modes, compress levels)
    for mode in ('RGB', 'RGBA', 'L', 'LA', 'P', '1', 'I;16'):
        for sz in ((100, 80), (3, 200), (257, 2)):
            if mode == 'I;16': im = Image.fromarray(rng.integers(0, 65536, sz[::-1]).astype(np.uint16))
            elif mode == 'P':
                im = Image.fromarray(rng.integers(0, 256, sz[::-1]).astype(np.uint8)).convert('L').convert('P', palette=Image.ADAPTIVE); 
            else: im = Image.fromarray(rng.integers(0, 256, sz[::-1] + ((len(mode),) if mode not in ('L', '1') else ())).astype(np.uint8), 'RGBA' if mode == 'RGBA' else ('RGB' if mode == 'RGB' else ('LA' if mode == 'LA' else 'L'))).convert(mode)
            p = os.path.join(TMP, 'pil.png'); im.save(p, optimize=True, compress_level=9)
            got, info = run_dec(p)
            if mode == 'I;16':
                a = np.array(im).astype(np.int64); exp = scale16(a)
                ok = got is not None and (got[..., 0] == exp).all() and (got[..., 3] == 255).all()
            else:
                exp = np.array(Image.open(p).convert('RGBA'))
                ok = got is not None and (got == exp).all()
            R.check('png pillow %s %s' % (mode, sz), ok, info[:80])
    # real image: noise + gradient, large IDAT stream with long matches, stored/fixed blocks
    for lvl in (0, 1, 9):
        im = Image.fromarray((np.add.outer(np.arange(300), np.arange(400)) % 256).astype(np.uint8)).convert('RGB')
        b = io.BytesIO(); im.save(b, 'PNG', compress_level=lvl); p = os.path.join(TMP, 'lv.png'); open(p, 'wb').write(b.getvalue())
        got, info = run_dec(p); R.check('png zlib level %d' % lvl, got is not None and (got[..., :3] == np.array(im)).all(), info[:80])
    # palette index out of range -> black, not entry 0
    arr = np.array([[[0, 1, 2, 3, 200]]]).reshape(1, 5, 1)
    p = os.path.join(TMP, 'oor.png'); write_png(p, arr, 3, 8, [[255, 0, 0], [0, 255, 0], [0, 0, 255]])
    got, info = run_dec(p); R.check('png palette index out of range', got is not None and (got[0, 3] == [0, 0, 0, 255]).all() and (got[0, 4] == [0, 0, 0, 255]).all() and (got[0, 0] == [255, 0, 0, 255]).all())
    # APNG: default image only
    arr = rng.integers(0, 256, (9, 9, 3)); p = os.path.join(TMP, 'apng.png'); write_png(p, arr, 2, 8, apng=True)
    got, info = run_dec(p); R.check('png apng default image', got is not None and (got[..., :3] == arr).all())
    # rejects: invalid colortype/depth combos, bad CRCs, bad adler, truncated, bad filter
    def mutate_file(src, fn):
        d = bytearray(open(src, 'rb').read()); fn(d); q = os.path.join(TMP, 'bad.png'); open(q, 'wb').write(bytes(d)); return q
    good = os.path.join(TMP, 'good.png'); arr = rng.integers(0, 256, (20, 20, 3)); write_png(good, arr, 2, 8)
    def fixcrc(d, ofs):                                    # recompute IHDR CRC
        d[ofs + 17:ofs + 21] = struct.pack('>I', zlib.crc32(bytes(d[ofs + 4:ofs + 17])) & 0xFFFFFFFF)
    def bad_combo(d): d[24] = 16; d[25] = 3; fixcrc(d, 8)
    def bad_combo2(d): d[24] = 4; d[25] = 2; fixcrc(d, 8)
    def bad_combo3(d): d[24] = 3; fixcrc(d, 8)
    def bad_crc(d): d[40] ^= 1
    def bad_ihdr_crc(d): d[30] ^= 1
    def bad_adler(d):                                      # flip a byte of adler inside IDAT then fix chunk crc
        i = d.rindex(b'IEND') - 4 - 4 - 1; d[i] ^= 0x55
        s = d.index(b'IDAT') - 4; ln = struct.unpack('>I', d[s:s + 4])[0]
        d[s + 8 + ln:s + 12 + ln] = struct.pack('>I', zlib.crc32(bytes(d[s + 4:s + 8 + ln])) & 0xFFFFFFFF)
    for nm, fn in (('combo16/pal', bad_combo), ('combo4/rgb', bad_combo2), ('combo rgb3', bad_combo3), ('idat crc', bad_crc), ('ihdr crc', bad_ihdr_crc), ('adler', bad_adler)):
        got, info = run_dec(mutate_file(good, fn)); R.check('png reject ' + nm, got is None, info[:60])
    d = open(good, 'rb').read()
    got, info = run_dec(good); R.check('png good baseline', got is not None)
    q = os.path.join(TMP, 'trunc.png'); open(q, 'wb').write(d[:len(d) // 2]); got, info = run_dec(q); R.check('png reject truncated', got is None)
    # crop vs fit semantics
    arr = rng.integers(0, 256, (50, 80, 4)); p = os.path.join(TMP, 'big.png'); write_png(p, arr, 6, 8, interlace=True)
    got, info = run_dec(p, 'crop', 30, 20); R.check('png crop (interlaced)', got is not None and (got == arr[:20, :30]).all(), info[:60])
    p2 = os.path.join(TMP, 'big2.png'); write_png(p2, arr, 6, 8)
    got, info = run_dec(p2, 'crop', 30, 20); R.check('png crop', got is not None and (got == arr[:20, :30]).all())
    for (pp, nm) in ((p, 'interlaced'), (p2, 'plain')):
        for (mw, mh) in ((40, 40), (80, 10), (7, 50), (79, 49), (1, 1)):
            got, info = run_dec(pp, 'fit', mw, mh)
            ok = got is not None and got.shape[0] <= mh and got.shape[1] <= mw
            if ok:
                ref = np.array(Image.fromarray(arr.astype(np.uint8), 'RGBA'))
                # exact area average reference (alpha-weighted)
                h2, w2 = got.shape[:2]; exp = area_ref(ref, w2, h2)
                dd = np.abs(got.astype(int) - exp.astype(int)).max(); ok = dd <= 1
                info = 'maxdiff %d dims %s' % (dd, got.shape)
            R.check('png fit %s %dx%d' % (nm, mw, mh), ok, info[:80])

def area_ref(img, dw, dh):
    """exact fractional area average, alpha weighted, float reference"""
    sh, sw = img.shape[:2]
    a = img.astype(np.float64)
    ypos = np.arange(sh)[:, None]; 
    def weights(sn, dn):
        w = np.zeros((dn, sn))
        for j in range(dn):
            lo, hi = j * sn / dn, (j + 1) * sn / dn
            for i in range(int(lo), min(sn, int(np.ceil(hi)))):
                w[j, i] = max(0, min(hi, i + 1) - max(lo, i))
        return w
    wy = weights(sh, dh); wx = weights(sw, dw)
    al = a[..., 3:4]
    pm = np.concatenate([a[..., :3] * al, al], -1)
    t = np.einsum('jy,yxc->jxc', wy, pm); t = np.einsum('ix,jxc->jic', wx, t)
    tot = sw * sh / (dw * dh)
    A = t[..., 3:4]
    col = np.where(A > 0, t[..., :3] / np.maximum(A, 1e-9), 0)
    alpha = A / tot
    return np.floor(np.concatenate([col, alpha], -1) + 0.5).clip(0, 255).astype(np.uint8)

def run_gzip(R):
    rng = np.random.default_rng(3)
    datas = [b'', b'a', b'hello world ' * 1000, bytes(rng.integers(0, 256, 70000).astype(np.uint8)), bytes(rng.integers(0, 4, 300000).astype(np.uint8)), b'\0' * 1000000]
    for i, d in enumerate(datas):
        for lvl in (0, 1, 9):
            z = gzip.compress(d, lvl); p = os.path.join(TMP, 'x.gz'); open(p, 'wb').write(z)
            if len(d) == 0: continue                      # gzip_inflate needs out_cap>0 and returns 0 bytes: fine either way
            out = run_gunzip(p, len(d) + 10)
            R.check('gzip %d lvl%d' % (i, lvl), out == d)
    d = datas[3]; z = gzip.compress(d, 6); p = os.path.join(TMP, 'x.gz'); open(p, 'wb').write(z)
    out = run_gunzip(p, 1000); R.check('gzip stops successfully at out_cap', out == d[:1000])
    open(p, 'wb').write(z[:-8] + b'\0' * 8); R.check('gzip bad crc rejected', run_gunzip(p, len(d) + 10) is None)
    open(p, 'wb').write(z[:-3]); R.check('gzip truncated trailer tolerated', run_gunzip(p, len(d) + 10) == d)
    open(p, 'wb').write(z[:len(z) // 2]); R.check('gzip truncated body rejected', run_gunzip(p, len(d) + 10) is None)
    open(p, 'wb').write(gzip.compress(b'abc' * 100) + gzip.compress(b'def' * 100)); R.check('gzip multi-member', run_gunzip(p, 5000) == b'abc' * 100 + b'def' * 100)
    import gzip as gz
    buf = io.BytesIO(); 
    with gz.GzipFile(fileobj=buf, mode='wb', filename='name.txt', mtime=5) as f: f.write(b'xyz' * 50)
    open(p, 'wb').write(buf.getvalue()); R.check('gzip with FNAME', run_gunzip(p, 500) == b'xyz' * 50)

if __name__ == '__main__':
    R = Results(); run(R); run_gzip(R)
    print('png/gzip: %d passed, %d failed' % (R.passed, len(R.failed)))
    sys.exit(1 if R.failed else 0)
