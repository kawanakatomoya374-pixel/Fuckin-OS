#!/usr/bin/env python3
"""Turns a built PSXFunkin tree (its tools run: `make -f Makefile.tools; make -f Makefile.tim; make -f Makefile.cht`) into the data directory the C-OS port reads:
   OUT/FILES.TXT     "<iso name> <path below OUT>" for every file of funkin.xml (names in lower case, '/' as separator, no ';1')
   OUT/OVERLAYS.TXT  "<overlay name>: <data file> ..."  the data stream of each overlay, in the order the game reads it (from the Makefile)
   OUT/...           the files themselves: textures (.tim), charts (.cht), character archives (.arc), music (.mus = the text description, plus the audio files)
Usage: prep_assets.py UPSTREAM_DIR OUT_DIR"""
import os, re, shutil, sys
import xml.etree.ElementTree as ET

up, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
files = []          # (iso name, relative path in out)
copied = {}

def copy_rel(src_rel):
    src = os.path.join(up, src_rel)
    if not os.path.exists(src):
        return None
    rel = src_rel[4:] if src_rel.startswith('iso/') else src_rel
    dst = os.path.join(out, rel)
    if rel not in copied:
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst); copied[rel] = True
    return rel

root = ET.parse(os.path.join(up, 'funkin.xml')).getroot()
def walk(node, prefix):
    for e in node:
        if e.tag == 'dir':
            walk(e, prefix + [e.get('name').lower()])
        elif e.tag == 'file':
            src = e.get('source')
            if not src or not src.startswith('iso/'):
                continue
            iso = '/'.join(prefix + [e.get('name').lower()])
            if iso.endswith('.exe'):
                files.append((iso, None)); continue          # the code of an overlay is linked into the program
            if iso.endswith('.mus') and not os.path.exists(os.path.join(up, src)):
                src = src[:-4] + '.txt'                          # the text description (the PlayStation tools turn it into the .mus stream)
            rel = copy_rel(src)
            if rel is None:
                print('missing', src); continue
            if iso.endswith('.mus'):
                # the description is a text file next to its audio files: copy those too
                d = os.path.dirname(src)
                for f in os.listdir(os.path.join(up, d)):
                    if f.lower().endswith(('.mp3', '.ogg')):
                        copy_rel(d + '/' + f)
            files.append((iso, rel))
tree = root.find('track/directory_tree')
walk(tree, [])
with open(os.path.join(out, 'FILES.TXT'), 'w') as f:
    for iso, rel in files:
        if rel:
            f.write('%s %s\n' % (iso, rel))

# overlays: iso/menu/menu.exe: Overlay.menu iso/menu/back.tim ...
mk = open(os.path.join(up, 'Makefile')).read().replace('\\\n', ' ')
with open(os.path.join(out, 'OVERLAYS.TXT'), 'w') as f:
    for m in re.finditer(r'^iso/([\w/]+\.exe):\s*(.*)$', mk, re.M):
        deps = [d for d in m.group(2).split() if d.startswith('iso/')]
        rels = []
        for d in deps:
            r = copy_rel(d)
            if r is None: print('missing overlay data', d)
            else: rels.append(r)
        f.write('%s: %s\n' % (m.group(1).lower(), ' '.join(rels)))
total = sum(os.path.getsize(os.path.join(dp, fn)) for dp, _, fns in os.walk(out) for fn in fns)
print('files', len(files), 'copied', len(copied), 'bytes', total)
