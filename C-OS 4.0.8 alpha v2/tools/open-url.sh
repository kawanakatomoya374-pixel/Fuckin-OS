#!/usr/bin/env sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if [ $# -eq 0 ]; then
    exec "$DIR/open-browser.sh"
fi

# Pass a URL, file path, or search query to the selected browser launcher.
exec "$DIR/open-browser.sh" "$@"
