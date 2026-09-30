#!/usr/bin/env python3
import argparse, os, struct, sys, zlib
S=512; MAGIC=0x43535350464F5354; VERSION=4; SECTORS=1048576; CAT=64; DATA=132; MAX=128; H=72; E=188; CS=64

def u(b,o): return struct.unpack_from('<Q',b,o)[0]
def crc(b,skip,n):
    x=zlib.crc32(b[:skip],0xffffffff); x=zlib.crc32(b[skip+n:],x); return (~x)&0xffffffff
def check(path):
    if os.path.getsize(path) < SECTORS*S: raise RuntimeError('image is smaller than 512 MiB')
    with open(path,'rb') as f:
        for sec,label in ((0,'primary'),(65,'backup')):
            f.seek(sec*S); c=bytearray(f.read(H+MAX*E))
            if len(c)!=H+MAX*E: raise RuntimeError(label+' catalog truncated')
            if [u(c,x) for x in (0,8,16,24,32,40)] != [MAGIC,VERSION,SECTORS,CAT,DATA,MAX]: raise RuntimeError(label+' header mismatch')
            saved=u(c,CS); struct.pack_into('<Q',c,CS,0)
            if saved != crc(c,CS,8): raise RuntimeError(label+' checksum mismatch')
            if sec==0: primary=c; entries=u(c,48)
        if u(primary,48) != sum(1 for i in range(MAX) if primary[H+i*E]): raise RuntimeError('used_files mismatch')
    print('storage image valid: %s (%d entries)'%(path,entries))

def main():
    p=argparse.ArgumentParser(); p.add_argument('--image',required=True); a=p.parse_args()
    try: check(a.image)
    except (OSError,RuntimeError,ValueError) as e: print('storage image invalid:',e,file=sys.stderr); return 1
    return 0
if __name__=='__main__': sys.exit(main())
