#!/usr/bin/env sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODE=${C_OS_BROWSER:-auto}

open_text_browser() {
    BIN="$DIR/../build/text_browser"
    if [ ! -x "$BIN" ]; then
        echo "build/text_browser not found. Run: make text-browser" >&2
        exit 1
    fi
    exec "$BIN" "$@"
}

open_min_browser() {
    "$DIR/open-min-browser.sh" "$@"
}

case "$MODE" in
    text|internal)
        open_text_browser "$@"
        ;;
    min)
        open_min_browser "$@"
        ;;
    auto|"")
        open_text_browser "$@"
        ;;
    *)
        echo "Unknown C_OS_BROWSER mode: $MODE (use auto, text, internal, or min)" >&2
        exit 2
        ;;
esac
