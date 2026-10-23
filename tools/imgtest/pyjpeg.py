"""Minimal JPEG *encoder* used to build streams Pillow cannot write: 4:4:0, 4:1:1, odd sampling
factors, non-interleaved multi-scan sequential, SOF1, 12-bit precision, DNL, restart-marker
damage.  Not optimised: flat Huffman tables, float DCT."""
import numpy as np, struct, math

ZZ = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]
DCT = np.array([[math.cos((2 * x + 1) * u * math.pi / 16) * (math.sqrt(0.125) if u == 0 else 0.5) for x in range(8)] for u in range(8)])

def fdct(blocks):  # (n,8,8) float -> coefficients
    return np.einsum('ux,nxy,vy->nuv', DCT, blocks, DCT)

class BitW:
    def __init__(s): s.out = bytearray(); s.acc = 0; s.n = 0
    def put(s, v, n):
        for i in range(n - 1, -1, -1):
            s.acc = (s.acc << 1) | ((v >> i) & 1); s.n += 1
            if s.n == 8: s.flush_byte()
    def flush_byte(s):
        b = s.acc & 255; s.out.append(b)
        if b == 0xFF: s.out.append(0)
        s.acc = 0; s.n = 0
    def align(s):
        while s.n: s.put(1, 1)

def flat_table(symbols):
    n = len(symbols); L = max(1, math.ceil(math.log2(n)) + 1) if n > 1 else 1
    bits = [0] * 16; bits[L - 1] = n
    return bits, list(symbols), L

def dht_seg(tc, th, bits, vals):
    d = bytes([(tc << 4) | th]) + bytes(bits) + bytes(vals)
    return b'\xff\xc4' + struct.pack('>H', len(d) + 2) + d

def encode(planes, W, H, samp, qts, *, precision=8, sof=0xC0, interleaved=True, restart=0, dnl=False, comp_ids=None,
           qsel=None, rgb_adobe=None, jfif=True, scan_groups=None, fill=0, extra_app=b'', corrupt_rst=None):
    """planes: list of full-resolution float arrays (H,W) in sample range. samp: [(h,v)...]."""
    nc = len(planes); hmax = max(h for h, v in samp); vmax = max(v for h, v in samp)
    mcux = -(-W // (8 * hmax)); mcuy = -(-H // (8 * vmax))
    comp_ids = comp_ids or list(range(1, nc + 1)); qsel = qsel or [0] + [1] * (nc - 1)
    center = 1 << (precision - 1); maxv = (1 << precision) - 1
    coefs = []; dims = []
    for c in range(nc):
        h, v = samp[c]; cw = -(-W * h // hmax); ch = -(-H * v // vmax)
        P = planes[c]
        fx, fy = hmax / h, vmax / v
        ys = np.minimum((np.arange(ch)[:, None] * fy + (fy - 1) / 2).astype(int), H - 1)
        xs = np.minimum((np.arange(cw)[None, :] * fx + (fx - 1) / 2).astype(int), W - 1)
        if float(fx).is_integer() and float(fy).is_integer():
            fx, fy = int(fx), int(fy); pad = np.pad(P, ((0, (-H) % fy), (0, (-W) % fx)), mode='edge')
            S = pad.reshape(pad.shape[0] // fy, fy, pad.shape[1] // fx, fx).mean((1, 3))[:ch, :cw]
        else: S = P[ys, xs]
        mwb, mhb = mcux * h, mcuy * v
        S = np.pad(S, ((0, mhb * 8 - ch), (0, mwb * 8 - cw)), mode='edge')
        blk = S.reshape(mhb, 8, mwb, 8).transpose(0, 2, 1, 3).reshape(-1, 8, 8) - center
        q = np.array(qts[qsel[c]], float)               # zigzag order
        qn = np.zeros(64); qn[ZZ] = q
        co = np.rint(fdct(blk).reshape(-1, 64) / qn).astype(int)
        coefs.append(co.reshape(mhb, mwb, 64)); dims.append((cw, ch, mwb, mhb, -(-cw // 8), -(-ch // 8)))
    # tables
    dcsyms = list(range(0, 16)); acsyms = sorted({0x00, 0xF0} | {(r << 4) | s for r in range(16) for s in range(1, 16)})
    dcb, dcv, dcl = flat_table(dcsyms); acb, acv, acl = flat_table(acsyms)
    dc_code = {s: i for i, s in enumerate(dcv)}; ac_code = {s: i for i, s in enumerate(acv)}
    def cat(v):
        v = abs(v); n = 0
        while v: v >>= 1; n += 1
        return n
    def enc_block(bw, blk, pred):
        zz = blk[ZZ]; d = int(zz[0]) - pred; s = cat(d)
        bw.put(dc_code[s], dcl)
        if s: bw.put(d if d >= 0 else d + (1 << s) - 1, s)
        run = 0; last = 63
        while last > 0 and zz[last] == 0: last -= 1
        for k in range(1, last + 1):
            v = int(zz[k])
            if v == 0: run += 1; continue
            while run > 15: bw.put(ac_code[0xF0], acl); run -= 16
            s = cat(v); bw.put(ac_code[(run << 4) | s], acl); bw.put(v if v >= 0 else v + (1 << s) - 1, s); run = 0
        if last < 63: bw.put(ac_code[0x00], acl)
        return int(zz[0])
    out = bytearray(b'\xff\xd8')
    if jfif: out += b'\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00'
    if rgb_adobe is not None: out += b'\xff\xee\x00\x0eAdobe\x00\x64\x00\x00\x00\x00' + bytes([rgb_adobe])
    out += extra_app
    for i, q in enumerate(qts):
        if precision == 8 and max(q) < 256: out += b'\xff\xdb' + struct.pack('>HB', 67, i) + bytes(q)
        else: out += b'\xff\xdb' + struct.pack('>HB', 131, 0x10 | i) + b''.join(struct.pack('>H', x) for x in q)
    out += b'\xff' + bytes([sof]) + struct.pack('>HBHHB', 8 + 3 * nc, precision, 0 if dnl else H, W, nc)
    for c in range(nc): out += bytes([comp_ids[c], (samp[c][0] << 4) | samp[c][1], qsel[c]])
    out += dht_seg(0, 0, dcb, dcv) + dht_seg(1, 0, acb, acv)
    if restart: out += b'\xff\xdd\x00\x04' + struct.pack('>H', restart)
    groups = scan_groups or ([list(range(nc))] if interleaved else [[c] for c in range(nc)])
    first = True
    for g in groups:
        out += b'\xff\xda' + struct.pack('>HB', 6 + 2 * len(g), len(g))
        for c in g: out += bytes([comp_ids[c], 0x00])
        out += bytes([0, 63, 0])
        bw = BitW(); preds = {c: 0 for c in g}; count = 0; rst = 0
        if len(g) == 1:
            c = g[0]; cw, ch, mwb, mhb, wb, hb = dims[c]
            units = [[(c, by, bx)] for by in range(hb) for bx in range(wb)]
        else:
            units = []
            for my in range(mcuy):
                for mx in range(mcux):
                    u = []
                    for c in g:
                        h, v = samp[c]
                        for sy in range(v):
                            for sx in range(h): u.append((c, my * v + sy, mx * h + sx))
                    units.append(u)
        for ui, u in enumerate(units):
            if restart and count == restart:
                bw.align(); bw.out += b'\xff' + bytes([0xD0 + (rst & 7)]); rst += 1; count = 0
                preds = {c: 0 for c in g}
            for (c, by, bx) in u: preds[c] = enc_block(bw, coefs[c][by, bx], preds[c])
            count += 1
        bw.align(); out += bw.out
        if fill: out += b'\xff' * fill
        if dnl and first: out += b'\xff\xdc\x00\x04' + struct.pack('>H', H)
        first = False
    out += b'\xff\xd9'
    return bytes(out), coefs

def std_q(scale=1.0):
    lum = [16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40, 26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51, 56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87, 95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99]
    return [max(1, int(round(x * scale))) for x in lum]

def ycc(rgb):
    r, g, b = [rgb[..., i].astype(float) for i in range(3)]
    return [0.299 * r + 0.587 * g + 0.114 * b, -0.168736 * r - 0.331264 * g + 0.5 * b + 128, 0.5 * r - 0.418688 * g - 0.081312 * b + 128]
