#!/usr/bin/env python3
"""Create an empty C-OS persistent storage image without Go."""
import argparse, os, struct, sys, zlib
SECTOR=512; MAGIC=0x43535350464F5354; VERSION=4; DISK_SECTORS=1048576
CATALOG_SECTOR=0; CATALOG_SECTORS=64; BACKUP=65; PASSWORD=129; DATA_START=132
MAX_FILES=128; ENTRY_SIZE=188; HEADER_SIZE=72; CATALOG_SIZE=HEADER_SIZE+MAX_FILES*ENTRY_SIZE

def put(b,o,v): struct.pack_into('<Q',b,o,v)
def crc(data,skip,length):
    x=zlib.crc32(data[:skip],0xffffffff); x=zlib.crc32(data[skip+length:],x); return (~x)&0xffffffff

def create(path,size_mb):
    if size_mb < 512: raise ValueError('C-OS storage requires at least 512 MiB')
    os.makedirs(os.path.dirname(os.path.abspath(path)),exist_ok=True)
    with open(path,'wb') as f: f.truncate(size_mb*1024*1024)
    c=bytearray(CATALOG_SIZE)
    for off,val in enumerate((MAGIC,VERSION,DISK_SECTORS,CATALOG_SECTORS,DATA_START,MAX_FILES,0,0,0)):
        put(c,off*8,val)
    put(c,64,0); put(c,64,crc(c,64,8))
    pwd=bytearray(80); put(pwd,0,MAGIC); put(pwd,72,0); put(pwd,72,crc(pwd,72,8))
    with open(path,'r+b') as f:
        f.seek(CATALOG_SECTOR*SECTOR); f.write(c)
        f.seek(BACKUP*SECTOR); f.write(c)
        f.seek(PASSWORD*SECTOR); f.write(pwd)
    print('storage image created:',path,'(%d MiB)'%size_mb)

def main():
    p=argparse.ArgumentParser(); p.add_argument('--out',default='build/storage.img'); p.add_argument('--size-mb',type=int,default=512)
    a=p.parse_args()
    try: create(a.out,a.size_mb)
    except Exception as e: print('storage image build failed:',e,file=sys.stderr); return 1
    return 0
if __name__=='__main__': sys.exit(main())
