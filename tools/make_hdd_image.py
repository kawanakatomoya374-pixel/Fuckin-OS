#!/usr/bin/env python3
"""Builds a bootable C-OS hard-disk / SSD image (raw, GPT) that boots on both legacy BIOS and UEFI:

  p1  BIOS boot partition   1 MiB   GRUB's core.img (BIOS boot of a GPT disk)
  p2  EFI System Partition  FAT32   \\EFI\\BOOT\\BOOTX64.EFI, /boot/grub (config, modules, theme), /boot/kernel.elf
  p3  C-OS data             rest    storage catalog + FAT32 user filesystem (formatted by the kernel on first boot)

The kernel finds p3 by its type GUID (DATA_GUID below, mirrored in src/drivers/disk/storage.c) and keeps all of its
persistent data inside it, so the boot partitions are never touched. Write the result to a drive with dd, or attach it
to QEMU (COS_BOOT_DISK=...). Needs: grub-mkimage, grub-bios-setup, mkfs.vfat, mtools. No root.

Secure Boot: with --secure-efi (the signed standalone BOOTX64.EFI that tools/build_secureboot_uefi.sh makes; it carries its own configuration
and the kernel) the ESP holds only that file (and the public certificate with --cert), no loose GRUB, config or kernel that the firmware would
not verify, and no BIOS boot code: the disk boots through UEFI with Secure Boot on, once the certificate is enrolled in the firmware's db.

usage: make_hdd_image.py --kernel build/kernel.boot.gz --boot-tree iso/boot --out build/c-os-disk.img [--size-mib 2560]
       make_hdd_image.py --secure-efi build/secureboot/BOOTX64.EFI [--cert build/secureboot/C-OS-secureboot-public.cer] --out build/c-os-disk-secure.img
"""
import argparse, os, struct, subprocess, sys, tempfile, zlib, shutil, uuid

SECTOR = 512
DATA_GUID = bytes([0x43, 0x4F, 0x53, 0x44, 0x41, 0x54, 0x41, 0x00, 0x80, 0x00, 0xC0, 0x5D, 0xA7, 0xA0, 0x00, 0x01])
BIOS_BOOT_GUID = uuid.UUID('21686148-6449-6E6F-744E-656564454649').bytes_le
ESP_GUID = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B').bytes_le
GRUB_I386 = '/usr/lib/grub/i386-pc'
GRUB_EFI = '/usr/lib/grub/x86_64-efi'

def run(*a, **k):
    r = subprocess.run(a, capture_output=True, text=True, **k)
    if r.returncode:
        sys.exit('command failed: %s\n%s%s' % (' '.join(a), r.stdout, r.stderr))
    return r

def entry(type_guid, first, last, name):
    return type_guid + uuid.uuid4().bytes_le + struct.pack('<QQQ', first, last, 0) + name.encode('utf-16le').ljust(72, b'\0')

def gpt(total, parts):
    entries = b''.join(entry(*p) for p in parts).ljust(128 * 128, b'\0')
    ecrc = zlib.crc32(entries)
    guid = uuid.uuid4().bytes_le
    def header(cur, backup, ent_lba):
        h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, cur, backup, 34, total - 34, guid, ent_lba, 128, 128, ecrc)
        crc = zlib.crc32(h)
        return (h[:16] + struct.pack('<I', crc) + h[20:]).ljust(SECTOR, b'\0')
    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack('<B3sB3sII', 0, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xAA'
    return bytes(mbr), header(1, total - 1, 2), entries, header(total - 1, 1, total - 33)

def build_grub_esp(a, esp, tmp, mc):
    run('mmd', '-i', esp, '::/boot', '::/boot/grub')
    mc('-s', *[os.path.join(a.boot_tree, n) for n in os.listdir(a.boot_tree) if n not in ('kernel.elf', 'kernel.elf.gz')], '::/boot/')
    mc(a.kernel, '::/boot/kernel.elf.gz' if a.kernel.endswith('.gz') else '::/boot/kernel.elf')
    # GRUB environment block (kernel update: trial / confirm / fallback state): fixed 1024 bytes, rewritten in place by GRUB and the kernel
    genv = os.path.join(tmp, 'grubenv')
    open(genv, 'wb').write(b'# GRUB Environment Block\n'.ljust(1024, b'#'))
    mc(genv, '::/boot/grub/grubenv')
    for plat in ('i386-pc', 'x86_64-efi'):
        src = os.path.join('/usr/lib/grub', plat)
        run('mmd', '-i', esp, '::/boot/grub/' + plat)
        mods = [os.path.join(src, n) for n in os.listdir(src) if n.endswith(('.mod', '.lst'))]
        for i in range(0, len(mods), 64): mc(*mods[i:i + 64], '::/boot/grub/%s/' % plat)
    efi = os.path.join(tmp, 'BOOTX64.EFI')
    mods = 'part_gpt part_msdos fat multiboot2 normal configfile echo test search all_video gfxterm font png efi_gop efi_uga'.split()
    run('grub-mkimage', '-O', 'x86_64-efi', '-p', '/boot/grub', '-o', efi, *mods)
    mc(efi, '::/EFI/BOOT/BOOTX64.EFI')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel')
    ap.add_argument('--boot-tree', help='directory whose contents become /boot (grub.cfg, themes, ...)')
    ap.add_argument('--secure-efi', help='signed standalone BOOTX64.EFI: a UEFI-only image for Secure Boot (no --kernel / --boot-tree)')
    ap.add_argument('--cert', help='public certificate (DER) to put next to it, for enrolling it in the firmware')
    ap.add_argument('--out', required=True)
    ap.add_argument('--size-mib', type=int, default=2560, help='total image size; the data partition needs >= 1.2 GiB')
    a = ap.parse_args()
    if a.secure_efi:
        if a.kernel or a.boot_tree: sys.exit('--secure-efi stands alone: the kernel and the configuration are inside the signed file')
        payload = a.secure_efi
    else:
        if not (a.kernel and a.boot_tree): sys.exit('--kernel and --boot-tree are required (or --secure-efi)')
        payload = a.kernel
    total = a.size_mib * 1024 * 1024 // SECTOR
    kernel_mib = (os.path.getsize(payload) + (1 << 20) - 1) >> 20
    esp_mib = max(160, kernel_mib + 40)
    p1 = (2048, 2048 + 2048 - 1)
    p2 = (p1[1] + 1, p1[1] + esp_mib * 2048)
    p3 = (p2[1] + 1, total - 34)
    if (p3[1] - p3[0] + 1) * SECTOR < 1300 * (1 << 20):
        sys.exit('image too small: the data partition must be at least 1300 MiB (raise --size-mib)')

    with tempfile.TemporaryDirectory() as tmp:
        # --- ESP contents
        esp = os.path.join(tmp, 'esp.img')
        with open(esp, 'wb') as f: f.truncate(esp_mib << 20)
        run('mkfs.vfat', '-F', '32', '-n', 'COSBOOT', esp)
        def mc(*args): run('mcopy', '-i', esp, *args)
        run('mmd', '-i', esp, '::/EFI', '::/EFI/BOOT')
        if a.secure_efi:
            mc(a.secure_efi, '::/EFI/BOOT/BOOTX64.EFI')
            if a.cert:
                run('mmd', '-i', esp, '::/EFI/COS'); mc(a.cert, '::/EFI/COS/C-OS-secureboot-public.cer')
        else:
            build_grub_esp(a, esp, tmp, mc)

        # --- disk image: GPT + ESP
        mbr, hdr, ents, bhdr = gpt(total, [(BIOS_BOOT_GUID, p1[0], p1[1], 'BIOS boot'), (ESP_GUID, p2[0], p2[1], 'C-OS boot'),
                                         (DATA_GUID, p3[0], p3[1], 'C-OS data')])
        with open(a.out, 'wb') as d:
            d.truncate(total * SECTOR)
            d.seek(0); d.write(mbr); d.write(hdr); d.write(ents)
            d.seek((total - 33) * SECTOR); d.write(ents); d.write(bhdr)
            d.seek(p2[0] * SECTOR)
            with open(esp, 'rb') as e: shutil.copyfileobj(e, d, 1 << 20)

        if not a.secure_efi:
            # --- BIOS boot: core.img on the BIOS boot partition + boot code in the MBR
            core = os.path.join(tmp, 'core.img'); gdir = os.path.join(tmp, 'g'); os.mkdir(gdir)
            run('grub-mkimage', '-O', 'i386-pc', '-p', '(hd0,gpt2)/boot/grub', '-o', core,
                'biosdisk', 'part_gpt', 'part_msdos', 'fat', 'multiboot2', 'normal', 'configfile', 'echo', 'test', 'search')
            shutil.copy(os.path.join(GRUB_I386, 'boot.img'), gdir); shutil.copy(core, gdir)
            devmap = os.path.join(tmp, 'device.map')
            out_abs = os.path.abspath(a.out)
            setup = '/usr/lib/grub/i386-pc/grub-bios-setup'
            open(devmap, 'w').write('(hd0)\t%s\n' % out_abs)
            r = subprocess.run([setup, '-d', 'g', '-m', 'device.map', '--skip-fs-probe', out_abs], cwd=tmp, capture_output=True, text=True)
            if r.returncode:
                # grub-bios-setup insists on identifying the host device that holds its image directory. Where that device
                # cannot be opened (containers), stage the images on a small loop-mounted ext2 filesystem instead (needs root).
                if os.geteuid() != 0 or not shutil.which('mke2fs') or not shutil.which('losetup'):
                    sys.exit('grub-bios-setup failed and the loop-mount fallback needs root:\n' + r.stderr)
                ext = os.path.join(tmp, 'stage.img'); mnt = os.path.join(tmp, 'mnt'); os.mkdir(mnt)
                with open(ext, 'wb') as f: f.truncate(8 << 20)
                run('mke2fs', '-q', '-F', ext)
                loop = run('losetup', '-f', '--show', ext).stdout.strip()
                try:
                    run('mount', loop, mnt)
                    try:
                        os.mkdir(os.path.join(mnt, 'g')); shutil.copy(os.path.join(gdir, 'boot.img'), os.path.join(mnt, 'g')); shutil.copy(core, os.path.join(mnt, 'g'))
                        open(devmap, 'w').write('(hd0)\t%s\n(hd1)\t%s\n' % (out_abs, loop))
                        run(setup, '-d', os.path.join(mnt, 'g'), '-m', devmap, '--skip-fs-probe', out_abs)
                    finally:
                        run('umount', mnt)
                finally:
                    run('losetup', '-d', loop)
    print('wrote %s  (%d MiB: BIOS boot %d MiB, ESP %d MiB, data %d MiB)' % (a.out, a.size_mib, 1, esp_mib, (p3[1] - p3[0] + 1) // 2048))

main()
