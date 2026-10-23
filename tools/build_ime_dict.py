#!/usr/bin/env python3
"""Builds userland/third_party/mozc_dict/cosime.dic (zlib-compressed) for the C-OS IME from Mozc's open-source dictionary.

Sources (BSD-3 Mozc, dictionary derived from IPAdic, see NOTICE.txt there), from
  https://github.com/google/mozc/tree/master/src/data/dictionary_oss   dictionary00..09.txt, id.def, connection_single_column.txt
Usage: build_ime_dict.py <dir with the downloaded files> [out file]

File = u32 raw length + zlib stream of the following (little endian, every section 4-byte aligned):
  char magic[8]="COSIME1"; u32 N, n_read, n_ent, pool_bytes, maxlen_chars, unknown_id
  u8 attached[N]            1 = the part of speech normally attaches to the previous word (particles, auxiliaries, suffixes, marks)
  i16 conn[N*N]             connection cost, row = right id of the left word, column = left id of the right word
  u32 read_off[n_read+1]    offsets of the NUL-terminated readings in the pool (sorted bytewise: binary search)
  u32 ent_start[n_read+1]   first entry of each reading (entries sorted by cost)
  u16 lid[n_ent], rid[n_ent], cost[n_ent]; u32 surf[n_ent]  (0xFFFFFFFF = surface equals reading, 0xFFFFFFFE = katakana of reading)
  pool
"""
import struct, sys, zlib, os, re

FONT = os.path.join(os.path.dirname(__file__), "..", "userland", "third_party", "NotoSansJP-subset.ttf")
src = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "userland", "third_party", "mozc_dict", "cosime.dic")
MAX_COST = 9800                       # drop the rare tail; the dictionary still has ~1.1M entries
KANA = re.compile(r"^[ぁ-ゖー゛゜]+$")

def kata(s):
    return "".join(chr(ord(c) + 0x60) if 0x3041 <= ord(c) <= 0x3096 else c for c in s)

# Candidates containing a character the system font cannot draw would show as an empty box: leave them out.
try:
    from fontTools.ttLib import TTFont
    cmap = set(TTFont(FONT).getBestCmap())
except Exception as e:
    sys.exit("fontTools and the font are needed to filter the dictionary (pip install fonttools): %s" % e)
def drawable(s):
    return all(ord(c) in cmap for c in s)

ids = {}
for l in open(os.path.join(src, "id.def"), encoding="utf-8"):
    i, f = l.rstrip("\n").split(" ", 1); ids[int(i)] = f
N = len(ids)
ATTACH = ("助詞", "助動詞", "動詞,接尾", "動詞,非自立", "形容詞,接尾", "形容詞,非自立", "名詞,接尾", "名詞,非自立", "記号,句点", "記号,読点", "記号,括弧閉")
attached = bytes(1 if any(ids[i].startswith(p) for p in ATTACH) else 0 for i in range(N))
unknown = next(i for i in range(N) if ids[i].startswith("名詞,一般,*,*,*,*,*"))

vals = open(os.path.join(src, "connection_single_column.txt")).read().split()
assert int(vals[0]) == N and len(vals) == 1 + N * N
conn = struct.pack("<%dh" % (N * N), *[int(v) for v in vals[1:]])

best = {}
for k in range(10):
    for l in open(os.path.join(src, "dictionary%02d.txt" % k), encoding="utf-8"):
        f = l.rstrip("\n").split("\t")
        if len(f) < 5: continue
        r, lid, rid, cost, s = f[0], int(f[1]), int(f[2]), int(f[3]), f[4]
        if cost > MAX_COST or not KANA.match(r) or not drawable(s): continue
        key = (r, s, lid, rid)
        if key not in best or cost < best[key]: best[key] = cost
byread = {}
for (r, s, lid, rid), cost in best.items():
    byread.setdefault(r, []).append((cost, lid, rid, s))
reads = sorted(byread, key=lambda r: r.encode("utf-8"))

pool = bytearray(); pool_index = {}
def pool_add(s):
    b = s.encode("utf-8") + b"\0"
    o = pool_index.get(b)
    if o is None: o = pool_index[b] = len(pool); pool.extend(b)
    return o
read_off, ent_start = [], []
lid_a, rid_a, cost_a, surf_a = [], [], [], []
for r in reads:
    read_off.append(pool_add(r)); ent_start.append(len(lid_a))
    for cost, lid, rid, s in sorted(byread[r]):
        lid_a.append(lid); rid_a.append(rid); cost_a.append(cost)
        surf_a.append(0xFFFFFFFF if s == r else 0xFFFFFFFE if s == kata(r) else pool_add(s))
read_off.append(len(pool)); ent_start.append(len(lid_a))
maxlen = max(len(r) for r in reads)

def pad(b):
    return b + b"\0" * (-len(b) % 4)
n_read, n_ent = len(reads), len(lid_a)
blob = b"COSIME1\0" + struct.pack("<6I", N, n_read, n_ent, len(pool), maxlen, unknown)
blob += pad(attached) + pad(conn)
blob += struct.pack("<%dI" % (n_read + 1), *read_off) + struct.pack("<%dI" % (n_read + 1), *ent_start)
blob += pad(struct.pack("<%dH" % n_ent, *lid_a)) + pad(struct.pack("<%dH" % n_ent, *rid_a)) + pad(struct.pack("<%dH" % n_ent, *cost_a))
blob += struct.pack("<%dI" % n_ent, *surf_a) + bytes(pool)
z = struct.pack("<I", len(blob)) + zlib.compress(blob, 9)      # u32 raw length, then the zlib stream
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, "wb").write(z)
print(f"readings={n_read} entries={n_ent} maxlen={maxlen} pool={len(pool)} raw={len(blob)} compressed={len(z)} unknown_id={unknown}")
