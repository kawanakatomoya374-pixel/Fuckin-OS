#!/usr/bin/env bash
# Builds the OVMF variable store with the C-OS development certificate enrolled as PK/KEK/db and Secure Boot on,
# for tools/run_qemu_c-os.sh --secure-boot. Needs virt-fw-vars (python3-virt-firmware) and the dev keys from
# tools/generate_secureboot_dev_keys.sh. The result lives under build/ and holds only the PUBLIC certificate.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CERT="${1:-$HOME/.local/share/c-os-secureboot/C-OS-dev-secureboot.pem}"
GUID=3b8bfa61-9c08-4c8a-8a04-3bceca0f4088
OUT="$ROOT/build/OVMF_VARS_4M.cosdev-secboot.latest.runtime.fd"
[[ -f "$CERT" ]] || { echo "certificate not found: $CERT (run tools/generate_secureboot_dev_keys.sh)" >&2; exit 1; }
virt-fw-vars --input /usr/share/OVMF/OVMF_VARS_4M.fd --output "$OUT" \
  --set-pk "$GUID" "$CERT" --add-kek "$GUID" "$CERT" --add-db "$GUID" "$CERT" --secure-boot
echo "wrote $OUT"
