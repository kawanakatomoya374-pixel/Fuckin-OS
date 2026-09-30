#!/usr/bin/env python3
"""Inject host files into a C-OS raw storage image.

Usage:
  tools/inject_storage.py --image build/storage.img \
      --add sample.mp3=/music/sample.mp3

The image format matches src/drivers/disk/storage.c. Both primary and backup
catalogs are updated and checksummed. Existing entries are replaced by a new
contiguous allocation; the old extent is intentionally left untouched because
this tool is for deterministic test images.
"""
import argparse
import os
import struct
import sys
import time
import zlib
from pathlib import PurePosixPath

SECTOR = 512
MAGIC = 0x43535350464F5354
VERSION = 4
DISK_SECTORS = 1048576
CATALOG_SECTOR = 0
CATALOG_SECTORS = 64
BACKUP_CATALOG_SECTOR = 65
DATA_START_SECTOR = 132
META_RING_SECTORS = 4096
MAX_FILES = 128
MAX_PATH = 128
HEADER_SIZE = 72
ENTRY_SIZE = 188
ENTRY_CHECKSUM_OFFSET = 180
CATALOG_CHECKSUM_OFFSET = 64
CATALOG_SIZE = HEADER_SIZE + MAX_FILES * ENTRY_SIZE


def get64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def put64(buf, off, value):
    struct.pack_into("<Q", buf, off, value)


def crc_skip(data, skip, length):
    crc = 0xFFFFFFFF
    crc = zlib.crc32(data[:skip], crc)
    crc = zlib.crc32(data[skip + length:], crc)
    return (~crc) & 0xFFFFFFFF


def normalize(path):
    path = path.replace("\\", "/")
    if not path.startswith("/"):
        path = "/" + path
    path = str(PurePosixPath(path))
    return "/" if path == "." else path


def entry(catalog, index):
    start = HEADER_SIZE + index * ENTRY_SIZE
    # A memoryview is required here: bytearray slicing returns a copy, which
    # would make injected entries disappear when the catalog is written.
    return memoryview(catalog)[start:start + ENTRY_SIZE]


def entry_path(e):
    return bytes(e[52:180]).split(b"\0", 1)[0].decode("utf-8", "replace")


def find_entry(catalog, path):
    for i in range(MAX_FILES):
        e = entry(catalog, i)
        if e[0] and entry_path(e) == path:
            return i
    return -1


def free_slot(catalog):
    for i in range(MAX_FILES):
        if entry(catalog, i)[0] == 0:
            return i
    return -1


def load_catalog(f):
    f.seek(CATALOG_SECTOR * SECTOR)
    c = bytearray(f.read(CATALOG_SIZE))
    if len(c) != CATALOG_SIZE:
        raise RuntimeError("catalog is truncated")
    header = [get64(c, off) for off in (0, 8, 16, 24, 32, 40)]
    expected = [MAGIC, VERSION, DISK_SECTORS, CATALOG_SECTORS, DATA_START_SECTOR, MAX_FILES]
    if header != expected:
        raise RuntimeError("storage catalog header mismatch: %r" % (header,))
    saved = get64(c, CATALOG_CHECKSUM_OFFSET)
    put64(c, CATALOG_CHECKSUM_OFFSET, 0)
    if saved != crc_skip(c, CATALOG_CHECKSUM_OFFSET, 8):
        raise RuntimeError("storage catalog checksum mismatch")
    put64(c, CATALOG_CHECKSUM_OFFSET, saved)
    return c


def ensure_dir(catalog, path, now):
    path = normalize(path)
    if path == "/" or find_entry(catalog, path) >= 0:
        return
    parent = normalize(str(PurePosixPath(path).parent))
    ensure_dir(catalog, parent, now)
    idx = free_slot(catalog)
    if idx < 0:
        raise RuntimeError("storage catalog has no free directory entry")
    e = entry(catalog, idx)
    e[:] = b"\0" * ENTRY_SIZE
    e[0], e[1] = 1, 1
    put64(e, 28, now)
    put64(e, 36, now)
    put64(e, 44, now)
    raw = path.encode("utf-8")[:MAX_PATH - 1]
    e[52:52 + len(raw)] = raw
    put64(e, ENTRY_CHECKSUM_OFFSET, crc_skip(e, ENTRY_CHECKSUM_OFFSET, 8))


def next_data_sector(catalog):
    result = DATA_START_SECTOR
    for i in range(MAX_FILES):
        e = entry(catalog, i)
        if not e[0] or e[1] != 0:
            continue
        result = max(result, get64(e, 4) + get64(e, 12))
    return result


def inject(image, specs):
    with open(image, "r+b") as f:
        if os.fstat(f.fileno()).st_size < DISK_SECTORS * SECTOR:
            raise RuntimeError("image is smaller than the C-OS storage format")
        catalog = load_catalog(f)
        now = int(time.time())
        next_sector = next_data_sector(catalog)
        added_bytes = 0
        for spec in specs:
            if "=" not in spec:
                raise RuntimeError("--add must be host-file=/path/in/C-OS: %s" % spec)
            host, dest = spec.split("=", 1)
            dest = normalize(dest)
            with open(host, "rb") as src:
                data = src.read()
            if not data:
                raise RuntimeError("refusing to inject an empty file: %s" % host)
            ensure_dir(catalog, normalize(str(PurePosixPath(dest).parent)), now)
            idx = find_entry(catalog, dest)
            if idx < 0:
                idx = free_slot(catalog)
            if idx < 0:
                raise RuntimeError("storage catalog has no free file entry")
            sectors = (len(data) + SECTOR - 1) // SECTOR
            if next_sector + sectors >= DISK_SECTORS - META_RING_SECTORS:
                raise RuntimeError("storage data area is full")
            padded = data + b"\0" * (sectors * SECTOR - len(data))
            f.seek(next_sector * SECTOR)
            f.write(padded)
            e = entry(catalog, idx)
            e[:] = b"\0" * ENTRY_SIZE
            e[0], e[1] = 1, 0
            put64(e, 4, next_sector)
            put64(e, 12, sectors)
            put64(e, 20, len(data))
            put64(e, 28, now)
            put64(e, 36, now)
            put64(e, 44, now)
            raw = dest.encode("utf-8")[:MAX_PATH - 1]
            e[52:52 + len(raw)] = raw
            put64(e, ENTRY_CHECKSUM_OFFSET, crc_skip(e, ENTRY_CHECKSUM_OFFSET, 8))
            print("injected %s -> %s (%d bytes, %d sectors)" %
                  (host, dest, len(data), sectors))
            next_sector += sectors
            added_bytes += len(data)
        used_files = sum(1 for i in range(MAX_FILES) if entry(catalog, i)[0])
        put64(catalog, 48, used_files)
        put64(catalog, 56, get64(catalog, 56) + added_bytes)
        put64(catalog, CATALOG_CHECKSUM_OFFSET, 0)
        put64(catalog, CATALOG_CHECKSUM_OFFSET, crc_skip(catalog, CATALOG_CHECKSUM_OFFSET, 8))
        f.seek(CATALOG_SECTOR * SECTOR)
        f.write(catalog)
        f.seek(BACKUP_CATALOG_SECTOR * SECTOR)
        f.write(catalog)
        verify = load_catalog(f)
        print("storage injection verified: %s (%d entries)" % (image, get64(verify, 48)))


def main():
    ap = argparse.ArgumentParser(description="inject files into a C-OS raw storage image")
    ap.add_argument("--image", default="build/storage.img")
    ap.add_argument("--add", action="append", required=True,
                    help="host-file=/path/in/C-OS; repeatable")
    args = ap.parse_args()
    try:
        inject(args.image, args.add)
    except (OSError, RuntimeError, ValueError) as exc:
        print("storage injection failed: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
