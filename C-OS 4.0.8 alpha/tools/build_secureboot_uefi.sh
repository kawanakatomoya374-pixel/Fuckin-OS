#!/usr/bin/env bash
# Build a Secure-Boot-verifiable, UEFI-first C-OS boot artifact.
#
# The signing key is supplied from outside the source tree. This script never
# copies a private key into the ESP/ISO and deliberately embeds the C-OS kernel
# inside the signed standalone GRUB image, so firmware verification of
# BOOTX64.EFI also covers the exact kernel payload GRUB can load.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  build_secureboot_uefi.sh --key PRIVATE_KEY --cert X509_CERT [options]

Required:
  --key PATH             PEM private key located outside the deliverable tree.
  --cert PATH            PEM X.509 certificate corresponding to --key.

Optional:
  --kernel PATH          Kernel ELF (default: build/kernel.elf).
  --config PATH          Secure standalone GRUB config
                         (default: src/boot/grub_secure.cfg).
  --out-dir PATH         Artifact directory (default: build/secureboot).
  --label LABEL          FAT volume label (default: COSSECURE).

Outputs:
  <out-dir>/BOOTX64.EFI              signed standalone GRUB
  <out-dir>/C-OS_4.0.8_alpha_secure.esp  FAT ESP image
  <out-dir>/C-OS_4.0.8_alpha_secure.iso  UEFI-first El Torito ISO
  <out-dir>/C-OS-secureboot-public.cer   public certificate only
  <out-dir>/manifest.txt                 hashes and verification records
EOF
}

key=""
cert=""
kernel="build/kernel.elf"
config="src/boot/grub_secure.cfg"
out_dir="build/secureboot"
label="COSSECURE"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --key) key="$2"; shift 2 ;;
        --cert) cert="$2"; shift 2 ;;
        --kernel) kernel="$2"; shift 2 ;;
        --config) config="$2"; shift 2 ;;
        --out-dir) out_dir="$2"; shift 2 ;;
        --label) label="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

for command in grub-mkstandalone sbsign sbverify openssl mkfs.vfat mmd mcopy xorriso sha256sum; do
    command -v "$command" >/dev/null || { echo "Required command missing: $command" >&2; exit 1; }
done
for path in "$key" "$cert" "$kernel" "$config"; do
    [[ -f "$path" ]] || { echo "Required file missing: $path" >&2; exit 1; }
done

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"
kernel="$(readlink -f "$kernel")"
config="$(readlink -f "$config")"
cert="$(readlink -f "$cert")"
key="$(readlink -f "$key")"

unsigned="$out_dir/BOOTX64.unsigned.efi"
signed="$out_dir/BOOTX64.EFI"
esp="$out_dir/C-OS_4.0.8_alpha_secure.esp"
iso="$out_dir/C-OS_4.0.8_alpha_secure.iso"
public_cert="$out_dir/C-OS-secureboot-public.cer"
staging="$out_dir/iso-root"
manifest="$out_dir/manifest.txt"

rm -f "$unsigned" "$signed" "$esp" "$iso" "$public_cert" "$manifest"
rm -rf "$staging"

# Build one PE/COFF image that contains GRUB, its immutable boot configuration
# and the exact ELF kernel. Under Secure Boot there is no loose, unsigned
# /boot/kernel.elf for this GRUB configuration to load.
grub-mkstandalone \
    -O x86_64-efi \
    --compress=xz \
    --modules="normal multiboot multiboot2 all_video gfxterm efi_gop efi_uga font terminal echo search search_fs_uuid part_gpt part_msdos fat iso9660 configfile" \
    -o "$unsigned" \
    "/boot/grub/grub.cfg=$config" \
    "/boot/kernel.elf=$kernel"

sbsign --key "$key" --cert "$cert" --output "$signed" "$unsigned"
sbverify --cert "$cert" "$signed"
openssl x509 -in "$cert" -outform DER -out "$public_cert"

# UEFI removable-media boot convention: EFI/BOOT/BOOTX64.EFI.
dd if=/dev/zero of="$esp" bs=1M count=64 status=none
mkfs.vfat -F 32 -n "$label" "$esp" >/dev/null
mmd -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/COS
mcopy -i "$esp" "$signed" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$esp" "$public_cert" ::/EFI/COS/C-OS-secureboot-public.cer

# A UEFI-first ISO is intentionally separate from the conventional hybrid ISO.
# The El Torito EFI image is the same signed ESP tested with OVMF.
mkdir -p "$staging/boot"
cp "$esp" "$staging/boot/esp.img"
printf '%s\n' \
    'C-OS Secure Boot media' \
    'BOOTX64.EFI is a signed standalone GRUB image with the kernel embedded.' \
    'The included certificate is public only; no private key is present.' \
    > "$staging/README.SECUREBOOT.txt"
xorriso -as mkisofs -R -J -joliet-long \
    -o "$iso" \
    -eltorito-alt-boot -e boot/esp.img -no-emul-boot \
    "$staging" >/dev/null

{
    echo 'C-OS Secure Boot artifact manifest'
    echo "signed_efi=$signed"
    echo "esp=$esp"
    echo "iso=$iso"
    echo "certificate=$public_cert"
    echo
    echo '[sbverify]'
    sbverify --list "$signed"
    echo
    echo '[El Torito]'
    xorriso -indev "$iso" -report_el_torito plain 2>/dev/null
    echo
    echo '[sha256]'
    sha256sum "$signed" "$esp" "$iso" "$public_cert"
} > "$manifest"

echo "[SECUREBOOT] signed EFI: $signed"
echo "[SECUREBOOT] ESP:        $esp"
echo "[SECUREBOOT] UEFI ISO:   $iso"
echo "[SECUREBOOT] manifest:   $manifest"
