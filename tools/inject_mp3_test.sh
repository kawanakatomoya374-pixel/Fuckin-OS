#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${ROOT}/build/storage_mp3_test.img"
RUN_QEMU=0
ADDS=()

usage() {
  cat <<'EOF'
Usage:
  tools/inject_mp3_test.sh [options] MP3_FILE [MP3_FILE ...]

Options:
  --image PATH       Output C-OS raw image (default: build/storage_mp3_test.img)
  --run              Launch the strict QEMU command after injection
  --help             Show this help

Each input MP3 is placed below /music using its basename. To choose an
explicit destination, use HOST_FILE=/music/path/file.mp3 as an argument.
The script never modifies build/storage.img unless --image points to it.
EOF
}

while (($#)); do
  case "$1" in
    --image) [[ $# -ge 2 ]] || { echo "--image requires a path" >&2; exit 2; }; IMAGE="$2"; shift 2 ;;
    --run) RUN_QEMU=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) ADDS+=("$1"); shift ;;
  esac
done

((${#ADDS[@]} > 0)) || { usage >&2; exit 2; }
mkdir -p "$(dirname "$IMAGE")"

# Recreate only a missing/invalid test image. A valid image is preserved so
# repeated invocations can add multiple files deterministically.
if [[ ! -f "$IMAGE" ]] || ! python3 "${ROOT}/tools/validate_storage_image.py" --image "$IMAGE" >/dev/null 2>&1; then
  python3 "${ROOT}/tools/pack_storage.py" --out "$IMAGE" --size-mb 512
fi

args=()
for item in "${ADDS[@]}"; do
  if [[ "$item" == *=/* ]]; then
    args+=(--add "$item")
  else
    base="$(basename "$item")"
    args+=(--add "$item=/music/$base")
  fi
done
python3 "${ROOT}/tools/inject_storage.py" --image "$IMAGE" "${args[@]}"
python3 "${ROOT}/tools/validate_storage_image.py" --image "$IMAGE"

echo "MP3 test image ready: ${IMAGE}"
echo "Use -drive file=${IMAGE},format=raw,if=none,id=disk in the QEMU command."
if ((RUN_QEMU)); then
  echo "--run requested; launching Secure Boot UEFI with storage image ${IMAGE}."
  COS_STORAGE="$IMAGE" exec "${ROOT}/tools/run_qemu_c-os.sh" --secure-boot --cpus 8
fi
