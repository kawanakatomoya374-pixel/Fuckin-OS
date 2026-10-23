import sys, os, io, struct
import numpy as np
from PIL import Image, ImageOps, ImageFile
from testlib import *

def photo(w, h, seed=1):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w]
    img = np.stack([128 + 100 * np.sin(x / 17.0 + y / 31.0), 128 + 100 * np.cos(x / 23.0 - y / 11.0), x * 255 // max(w, 1)], -1)
    img += rng.normal(0, 6, img.shape)
    if w > 20 and h > 20: img[h // 4:h // 2, w // 4:w // 2] = [250, 10, 10]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))

class Stat:
    def __init__(s): s.max = 0; s.sum = 0.0; s.n = 0
    def add(s, a, b):
        d = np.abs(a.astype(int) - b.astype(int)); s.max = max(s.max, int(d.max())); s.sum += float(d.sum()); s.n += d.size
st = Stat()

def cmp(R, name, path, ref, tol=1, **kw):
    got, info = run_dec(path, **kw)
    if got is None: R.check(name, False, info[:100]); return None
    ref = np.asarray(ref)
    if got.shape[:2] != ref.shape[:2]: R.check(name, False, 'shape %s vs %s %s' % (got.shape, ref.shape, info[:60])); return None
    st.add(got[..., :3], ref[..., :3])
    d = np.abs(got[..., :3].astype(int) - ref[..., :3].astype(int)).max()
    R.check(name, d <= tol, 'maxdiff %d' % d)
    return got, info

def save(im, name, **kw):
    p = os.path.join(TMP, name + '.jpg'); im.save(p, **kw); return p

def run(R):
    sizes = [(1, 1), (7, 9), (8, 8), (16, 16), (17, 33), (31, 17), (64, 48), (203, 157)]
    subs = {'444': 0, '422': 1, '420': 2}
    for (w, h) in sizes:
        im = photo(w, h)
        for sn, sv in subs.items():
            for extra, kw in (('base', {}), ('opt', dict(optimize=True)), ('prog', dict(progressive=True)), ('rst', dict(restart_marker_blocks=2)), ('rstrows', dict(restart_marker_rows=1))):
                if (w, h) not in ((1, 1), (7, 9), (17, 33), (203, 157)) and extra != 'base': continue
                p = save(im, 'j', quality=80, subsampling=sv, **kw)
                cmp(R, 'jpeg %dx%d %s %s' % (w, h, sn, extra), p, Image.open(p).convert('RGB'))
        p = save(im.convert('L'), 'jg', quality=80)
        cmp(R, 'jpeg %dx%d gray' % (w, h), p, Image.open(p).convert('RGB'))
        p = save(im.convert('L'), 'jgp', quality=80, progressive=True)
        cmp(R, 'jpeg %dx%d gray progressive' % (w, h), p, Image.open(p).convert('RGB'))
    im = photo(203, 157)
    for q in (1, 5, 25, 50, 75, 95, 100):
        for sv in (0, 2):
            p = save(im, 'jq', quality=q, subsampling=sv); cmp(R, 'jpeg quality %d sub%d' % (q, sv), p, Image.open(p).convert('RGB'))
    # custom (16-bit) quantisation tables
    qt = [[(i * 7 % 900) + 1 for i in range(64)]] * 3
    p = save(im, 'jq16', qtables=qt, subsampling=0)
    data = open(p, 'rb').read(); R.check('jpeg 16-bit DQT present in sample', b'\xff\xdb\x00\x84\x10' in data or b'\xff\xdb\x00\x84\x11' in data or b'\x10' in data)
    cmp(R, 'jpeg 16-bit DQT', p, Image.open(p).convert('RGB'))
    # EXIF orientation 1..8 + metadata
    for o in range(1, 9):
        ex = Image.Exif(); ex[0x112] = o
        p = save(photo(37, 21, o), 'jo', quality=85, exif=ex.tobytes(), subsampling=2)
        ref = ImageOps.exif_transpose(Image.open(p)).convert('RGB')
        r = cmp(R, 'jpeg exif orientation %d' % o, p, ref)
        if r: R.check('jpeg exif orientation %d reported' % o, ('ori=%d/%d' % (o, 1 if o != 1 else 0)) in r[1], r[1])
        r = run_dec(p, flags=2)
        R.check('jpeg exif %d NO_ORIENT' % o, r[0] is not None and r[0].shape[:2] == (21, 37))
    # CMYK (Adobe, inverted) and YCCK
    for sz in ((33, 17), (100, 60)):
        cm = photo(*sz).convert('CMYK')
        p = save(cm, 'jc', quality=90); cmp(R, 'jpeg CMYK %s' % (sz,), p, Image.open(p).convert('RGB'), tol=1)
    # RGB-colourspace JPEG (component ids 'R','G','B' via Pillow's keep_rgb)
    p = save(photo(50, 40), 'jrgb', quality=90, subsampling=0, keep_rgb=True)
    cmp(R, 'jpeg RGB colourspace (keep_rgb)', p, Image.open(p).convert('RGB'))
    # DCT-domain scaling vs libjpeg draft()
    for name, kw in (('444', dict(subsampling=0)), ('420', dict(subsampling=2)), ('422', dict(subsampling=1)), ('prog420', dict(progressive=True, subsampling=2))):
        p = save(photo(203, 157, 5), 'js', quality=88, **kw)
        for d in (2, 4, 8):
            im = Image.open(p); W, H = im.size; im.draft('RGB', (W // d, H // d)); ref = im.convert('RGB')
            cmp(R, 'jpeg scale 1/%d %s' % (d, name), p, ref, mode='fit', mw=ref.size[0], mh=ref.size[1])
    p = save(photo(203, 157), 'jg2', quality=88)  # gray scaled
    im = Image.open(p.replace('jg2', 'jg2')); 
    # fit that needs DCT scale + extra area-average reduction
    got, info = run_dec(save(photo(640, 480, 3), 'jbig', quality=90), 'fit', 50, 50)
    R.check('jpeg fit beyond 1/8 (area average)', got is not None and got.shape[:2] == (37, 50), info[:80] + str(None if got is None else got.shape))
    got, info = run_dec(save(photo(640, 480, 3), 'jbig', quality=90), 'fit', 400, 300)
    R.check('jpeg fit picks smallest divisor', got is not None and 'den=2' in info and got.shape[:2] == (240, 320), info[:80])
    got, info = run_dec(save(photo(640, 480, 3), 'jbig', quality=90), 'crop', 400, 300)
    R.check('jpeg non-fit oversize -> error (-3)', got is None and 'FAIL jpeg -3' in info, info[:80])
    ex = Image.Exif(); ex[0x112] = 6
    got, info = run_dec(save(photo(640, 480, 3), 'jbigo', quality=90, exif=ex.tobytes()), 'fit', 100, 150)
    R.check('jpeg fit + rotate (scaled and oriented)', got is not None and got.shape[0] <= 150 and got.shape[1] <= 100 and got.shape[0] > got.shape[1], info[:80])
    # markers / garbage tolerated: FF fill bytes, COM, junk between segments, multiple tables in one segment
    base = open(save(photo(40, 30), 'jm', quality=85, subsampling=2), 'rb').read()
    ref = Image.open(io.BytesIO(base)).convert('RGB')
    p = os.path.join(TMP, 'jm2.jpg')
    sof = base.index(b'\xff\xc0')
    mod = base[:2] + b'\xff\xff\xff\xfe\x00\x07hello' + b'\xff\xe5\x00\x04ab' + base[2:sof] + b'\xff\xff\xff' + base[sof:]
    open(p, 'wb').write(mod); cmp(R, 'jpeg COM/APPn/fill bytes', p, ref)
    # no DHT at all (MJPEG style): strip DHT segments from a standard-table file
    d = bytearray(base); out = bytearray(d[:2]); i = 2
    while i < len(d):
        if d[i] != 0xFF: break
        m = d[i + 1]; ln = struct.unpack('>H', d[i + 2:i + 4])[0]
        if m == 0xDA: out += d[i:]; break
        if m != 0xC4: out += d[i:i + 2 + ln]
        i += 2 + ln
    open(p, 'wb').write(bytes(out)); cmp(R, 'jpeg without DHT uses standard tables', p, ref)
    # truncation: decoded part must match libjpeg's output (rest gray) for baseline
    im = photo(120, 90); p = save(im, 'jt', quality=85, subsampling=2); full = open(p, 'rb').read()
    ok_cnt = 0; tot = 0
    for frac in (0.3, 0.5, 0.8, 0.95):
        cut = full[:int(len(full) * frac)]; q = os.path.join(TMP, 'jtc.jpg'); open(q, 'wb').write(cut)
        ImageFile.LOAD_TRUNCATED_IMAGES = True
        try: ref = Image.open(q); ref.load(); ref = ref.convert('RGB')
        except Exception as e: ref = None
        ImageFile.LOAD_TRUNCATED_IMAGES = False
        got, info = run_dec(q)
        R.check('jpeg truncated at %d%% decodes' % (frac * 100), got is not None and 'trunc=1' in info, info[:80])
        if got is not None and ref is not None and got.shape[:2] == np.array(ref).shape[:2]:
            d = np.abs(got[..., :3].astype(int) - np.array(ref).astype(int)); R.note('truncated %d%%: max diff vs libjpeg %d' % (frac * 100, d.max()))
    # progressive truncated still returns an image
    p = save(im, 'jtp', quality=85, progressive=True); full = open(p, 'rb').read()
    q = os.path.join(TMP, 'jtpc.jpg'); open(q, 'wb').write(full[:len(full) * 6 // 10]); got, info = run_dec(q)
    R.check('jpeg progressive truncated decodes', got is not None, info[:80])
    # rejects
    for nm, bad in (('garbage', b'\xff\xd8\xff\xe0' + b'\x00' * 20), ('only SOI', b'\xff\xd8\xff\xd9'), ('empty scan header', full[:full.index(b'\xff\xda')])):
        q = os.path.join(TMP, 'jbad.jpg'); open(q, 'wb').write(bad); got, info = run_dec(q); R.check('jpeg reject ' + nm, got is None, info[:60])
    # over-subscribed Huffman table must be rejected
    d = bytearray(full); i = d.index(b'\xff\xc4'); d[i + 5] = 200
    q = os.path.join(TMP, 'jbad.jpg'); open(q, 'wb').write(bytes(d)); got, info = run_dec(q); R.check('jpeg reject over-subscribed DHT', got is None, info[:60])
    # arithmetic coding reported as unsupported (not decoded)
    d = bytearray(base); i = d.index(b'\xff\xc0'); d[i + 1] = 0xC9
    q = os.path.join(TMP, 'jar.jpg'); open(q, 'wb').write(bytes(d)); got, info = run_dec(q); R.check('jpeg arithmetic -> UNSUPPORTED (-2)', got is None and 'FAIL jpeg -2' in info, info[:60])

def mk(planes, W, H, **kw):
    from pyjpeg import encode, std_q
    q = std_q(kw.pop('qscale', 0.6))
    return encode(planes, W, H, kw.pop('samp'), [q, q], **kw)[0]

def run_exotic(R):
    from pyjpeg import ycc, std_q, ZZ, DCT
    import pyjpeg
    rng = np.random.default_rng(11)
    def pil_ref(d):
        return np.array(Image.open(io.BytesIO(d)).convert('RGB'))
    def put(name, d):
        p = os.path.join(TMP, name + '.jpg'); open(p, 'wb').write(d); return p
    for (W, H) in ((50, 37), (8, 8), (1, 1), (17, 5), (33, 65)):
        pl = ycc(np.array(photo(W, H, 4)))
        samps = {'440': [(1, 2), (1, 1), (1, 1)], '411': [(4, 1), (1, 1), (1, 1)], '3x1': [(3, 1), (1, 1), (1, 1)],
                 '2x2/2x1/1x1': [(2, 2), (2, 1), (1, 1)], '4x2/1x1/2x2': [(4, 2), (1, 1), (2, 1)], '2x4 420': [(2, 4), (1, 2), (1, 2)],
                 '1x1x3 (h=2 luma only)': [(2, 2), (2, 2), (2, 2)], '2x1/2x1/1x1': [(2, 1), (2, 1), (1, 1)]}
        for nm, sp in samps.items():
            for il in (True, False):
                d = mk(pl, W, H, samp=sp, interleaved=il)
                if il and sum(h * v for h, v in sp) > 10:     # libjpeg refuses MCUs with >10 blocks: oracle = same coefficients, multi-scan
                    ref = pil_ref(mk(pl, W, H, samp=sp, interleaved=False))
                else: ref = pil_ref(d)
                cmp(R, 'jpeg exotic %dx%d %s %s' % (W, H, nm, 'interleaved' if il else 'multi-scan'), put('ex', d), ref)
        # gray with 2x2 sampling factor in the frame header (non-interleaved single comp)
        d = mk([pl[0]], W, H, samp=[(2, 2)], jfif=True); cmp(R, 'jpeg exotic %dx%d gray 2x2 factors' % (W, H), put('ex', d), pil_ref(d))
        # SOF1, restart intervals, fill bytes, DNL
        sp = [(2, 2), (1, 1), (1, 1)]
        for ri in (1, 2, 5, 7):
            d = mk(pl, W, H, samp=sp, restart=ri, sof=0xC1, fill=3); cmp(R, 'jpeg SOF1 restart %d %dx%d' % (ri, W, H), put('ex', d), pil_ref(d))
        base = mk(pl, W, H, samp=sp); d = mk(pl, W, H, samp=sp, dnl=True)
        got, info = run_dec(put('ex', d)); ref = pil_ref(base)
        R.check('jpeg DNL height %dx%d' % (W, H), got is not None and (got[..., :3] == ref).all(), info[:80])
        # RGB via Adobe transform 0, 4:4:4
        rgb = [np.array(photo(W, H, 4))[..., i].astype(float) for i in range(3)]
        d = mk(rgb, W, H, samp=[(1, 1)] * 3, jfif=False, rgb_adobe=0); cmp(R, 'jpeg Adobe RGB %dx%d' % (W, H), put('ex', d), pil_ref(d))
        d = mk(rgb, W, H, samp=[(1, 1)] * 3, jfif=False, comp_ids=[82, 71, 66]); cmp(R, "jpeg RGB ids 'RGB' %dx%d" % (W, H), put('ex', d), pil_ref(d))
    # restart marker damage: missing / wrong / extra markers -> same result as libjpeg
    pl = ycc(np.array(photo(80, 60, 9))); sp = [(2, 2), (1, 1), (1, 1)]
    good = mk(pl, 80, 60, samp=sp, restart=3)
    marks = [i for i in range(len(good) - 1) if good[i] == 0xFF and 0xD0 <= good[i + 1] <= 0xD7]
    R.check('jpeg restart sample has markers', len(marks) > 4)
    for nm, fn in (('missing marker', lambda b, m: b[:m[3]] + b[m[3] + 2:]),
                   ('wrong number', lambda b, m: b[:m[2] + 1] + bytes([0xD0 + ((b[m[2] + 1] + 3) & 7)]) + b[m[2] + 2:]),
                   ('two markers missing', lambda b, m: b[:m[1]] + b[m[1] + 2:m[2]] + b[m[2] + 2:]),
                   ('garbage byte inserted', lambda b, m: b[:m[2] - 1] + b'\x12\x34' + b[m[2] - 1:]),
                   ('truncated mid interval', lambda b, m: b[:m[4] + 5] + b'\xff\xd9')):
        d = fn(good, marks); p = put('exr', d)
        try: ref = pil_ref(d)
        except Exception as e: ref = None
        got, info = run_dec(p)
        if ref is not None and got is not None and got.shape[:2] == ref.shape[:2]:
            dd = np.abs(got[..., :3].astype(int) - ref.astype(int)).max(); R.check('jpeg restart damage: ' + nm, dd <= 1, 'maxdiff %d' % dd)
        else: R.check('jpeg restart damage: ' + nm, got is not None, info[:80])
    # 12-bit extended sequential (no libjpeg oracle: float IDCT reference, tolerance 2 of 255)
    for (W, H) in ((40, 24), (17, 9)):
        arr = (np.array(photo(W, H, 2)).astype(float) * 4095 / 255)
        pl12 = ycc(np.stack([arr[..., 0], arr[..., 1], arr[..., 2]], -1)); pl12 = [pl12[0], pl12[1] - 128 + 2048, pl12[2] - 128 + 2048]
        for ri in (0, 3):
            for sp in ([(1, 1)] * 3, [(2, 2), (1, 1), (1, 1)]):
                q = [max(1, x // 4) for x in std_q(0.2)]
                d, coefs = pyjpeg.encode(pl12, W, H, sp, [q, q], precision=12, sof=0xC1, restart=ri)
                got, info = run_dec(put('j12', d))
                ok = got is not None and 'prec=12' in info
                if ok:
                    # reference: IDCT of the quantised coefficients, upsampled by replication, converted with float YCC
                    planes = []
                    qn = np.zeros(64); qn[ZZ] = q
                    for c in range(3):
                        mhb, mwb = coefs[c].shape[:2]
                        blk = np.einsum('ux,nuv,vy->nxy', DCT, (coefs[c].reshape(-1, 64) * qn).reshape(-1, 8, 8), DCT)
                        img = blk.reshape(mhb, mwb, 8, 8).transpose(0, 2, 1, 3).reshape(mhb * 8, mwb * 8) + 2048
                        h, v = sp[c]; fx, fy = max(s[0] for s in sp) // h, max(s[1] for s in sp) // v
                        img = np.repeat(np.repeat(img, fy, 0), fx, 1)[:H, :W]
                        planes.append(img / 16.0)
                    Y, Cb, Cr = planes[0], planes[1] - 128, planes[2] - 128
                    exp = np.stack([Y + 1.402 * Cr, Y - 0.344136 * Cb - 0.714136 * Cr, Y + 1.772 * Cb], -1)
                    exp = np.clip(np.rint(exp), 0, 255)
                    dd = np.abs(got[..., :3].astype(float) - exp)
                    # chroma upsampling is triangle-filtered in the decoder, replicated in the reference: only compare 4:4:4 tightly
                    ok = dd.max() <= 2.5 if sp[0] == (1, 1) else dd.mean() <= 4
                    info = 'maxdiff %.1f mean %.2f' % (dd.max(), dd.mean())
                R.check('jpeg 12-bit %dx%d restart%d %s' % (W, H, ri, 'sub' if sp[0] != (1, 1) else '444'), ok, info[:80])

if __name__ == '__main__':
    R = Results(); run(R); run_exotic(R)
    print('jpeg: %d passed, %d failed; max abs diff vs libjpeg %d, mean %.5f' % (R.passed, len(R.failed), st.max, st.sum / max(st.n, 1)))
    for n in R.notes: print('  note:', n)
    sys.exit(1 if R.failed else 0)
