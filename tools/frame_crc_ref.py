#!/usr/bin/env python3
"""Reference per-frame CRC32s (Y/U/V) for a video, in the same format the Media
app prints when built with VALIDATION_FRAME_CRC=1:

    [FRAMECRC] <n> <w>x<h> <bit_depth> y=<crc> u=<crc> v=<crc>

Usage: frame_crc_ref.py <video.mp4 | embedded asset .c> [--diff serial.log]
Decodes with the host ffmpeg to 4:2:0 at the source's own depth (8/10/12-bit, little endian) so the output can be compared byte-for-byte with the device.
"""
import re, subprocess, sys, tempfile, zlib, os, json

def load(path):
    if path.endswith('.c'):
        data = bytes(int(h, 16) for h in re.findall(r'0x([0-9A-Fa-f]{2})', open(path).read()))
        fd, p = tempfile.mkstemp(suffix='.mp4'); os.write(fd, data); os.close(fd)
        return p
    return path

def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    src = load(args[0])
    probe = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
        '-show_entries', 'stream=width,height,pix_fmt,bits_per_raw_sample', '-of', 'json', src]))['streams'][0]
    w, h = probe['width'], probe['height']
    pf = probe['pix_fmt']
    depth = 12 if '12' in pf else 10 if '10' in pf else 8
    deep = depth > 8
    out_fmt = {8: 'yuv420p', 10: 'yuv420p10le', 12: 'yuv420p12le'}[depth]
    bps = 2 if deep else 1
    # REF_ALL_FRAMES=1 keeps frames an edit list would trim (the device prints every decoded frame)
    pre = ['-ignore_editlist', '1'] if os.environ.get('REF_ALL_FRAMES') else []
    post = ['-fps_mode', 'passthrough'] if pre else []
    raw = subprocess.check_output(['ffmpeg', '-v', 'error'] + pre + ['-i', src] + post + [ '-pix_fmt', out_fmt, '-f', 'rawvideo', '-'])
    cw, ch = (w + 1) // 2, (h + 1) // 2
    sizes = [w * h * bps, cw * ch * bps, cw * ch * bps]
    fs = sum(sizes)
    lines = []
    for n in range(len(raw) // fs):
        f = raw[n * fs:(n + 1) * fs]
        y, u, v = f[:sizes[0]], f[sizes[0]:sizes[0] + sizes[1]], f[sizes[0] + sizes[1]:]
        lines.append('[FRAMECRC] %d %dx%d %d y=%08x u=%08x v=%08x' % (
            n, w, h, depth, zlib.crc32(y) & 0xffffffff, zlib.crc32(u) & 0xffffffff, zlib.crc32(v) & 0xffffffff))
    if len(args) >= 3 and args[1] == '--diff':
        # The serial console can interleave another task's output into a
        # line, so match [FRAMECRC] anywhere in it and key on the frame number.
        import re
        pat = re.compile(r'\[FRAMECRC\] (\d+) \d+x\d+ \d+ y=[0-9a-f]{8} u=[0-9a-f]{8} v=[0-9a-f]{8}')
        devmap = {}
        for l in open(args[2], errors='replace'):
            m = pat.search(l)
            if m:
                devmap[int(m.group(1))] = m.group(0)
        dev = list(devmap.values())
        bad = 0
        for i, ref in enumerate(lines):
            d = devmap.get(i, '(missing)')
            if d != ref:
                bad += 1
                print('MISMATCH frame %d\n  ref: %s\n  dev: %s' % (i, ref, d))
        print('%d/%d frames match (%d device lines)' % (len(lines) - bad, len(lines), len(dev)))
        sys.exit(1 if bad else 0)
    print('\n'.join(lines))

main()
