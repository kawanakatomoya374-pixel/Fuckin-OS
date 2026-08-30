#!/usr/bin/env bash
# Generate a development Secure Boot signing key outside the C-OS source tree.
# The resulting private key must never be committed, copied into an ESP/ISO,
# or used as a production trust anchor.
set -euo pipefail

key_dir="${1:-${HOME}/.local/share/c-os-secureboot}"
case "$key_dir" in
    "$HOME"/*) ;;
    *) echo "Refusing key directory outside HOME: $key_dir" >&2; exit 2 ;;
esac

umask 077
mkdir -p "$key_dir"
chmod 700 "$key_dir"
key="$key_dir/C-OS-dev-secureboot.key"
cert="$key_dir/C-OS-dev-secureboot.pem"
cer="$key_dir/C-OS-dev-secureboot.cer"
esl="$key_dir/C-OS-dev-secureboot.esl"

if [[ ! -f "$key" || ! -f "$cert" ]]; then
    openssl req -new -x509 -newkey rsa:3072 -nodes \
        -keyout "$key" -out "$cert" -days 3650 -sha256 \
        -subj '/CN=C-OS Development Secure Boot/' \
        -addext 'basicConstraints=critical,CA:FALSE' \
        -addext 'keyUsage=critical,digitalSignature' \
        -addext 'extendedKeyUsage=codeSigning'
fi
openssl x509 -in "$cert" -outform DER -out "$cer"
# A deterministic owner GUID distinguishes this development certificate in db.
cert-to-efi-sig-list -g 3b8bfa61-9c08-4c8a-8a04-3bceca0f4088 "$cert" "$esl"
chmod 600 "$key"
chmod 644 "$cert" "$cer" "$esl"

printf 'Private key (keep outside deliverables): %s\n' "$key"
printf 'Public PEM: %s\nPublic DER: %s\nPublic ESL: %s\n' "$cert" "$cer" "$esl"
