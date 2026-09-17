#!/usr/bin/env sh
set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MIN_DIR=${MIN_DIR:-}

if [ -z "$MIN_DIR" ]; then
    if [ -d "$DIR/../min-master" ]; then
        MIN_DIR="$DIR/../min-master"
    elif [ -d "$DIR/../../min-master" ]; then
        MIN_DIR="$DIR/../../min-master"
    fi
fi

if [ -z "$MIN_DIR" ] || [ ! -d "$MIN_DIR" ]; then
    echo "MIN_DIR not set and min-master was not found next to the OS source tree." >&2
    echo "Set MIN_DIR=/path/to/min-master to use Min as the browser." >&2
    exit 1
fi

if [ ! -f "$MIN_DIR/package.json" ]; then
    echo "MIN_DIR does not look like a Min source tree: $MIN_DIR" >&2
    exit 1
fi

URL="${1:-https://www.google.com/}"

# Build once if the generated entrypoints are missing.
if [ ! -d "$MIN_DIR/node_modules" ] || [ ! -f "$MIN_DIR/main.build.js" ]; then
    echo "Min does not look built yet; running npm install/build flow from: $MIN_DIR" >&2
    (
        cd "$MIN_DIR"
        if [ ! -d node_modules ]; then
            npm install
        fi
        npm run build
    )
fi

cd "$MIN_DIR"
exec npm run startElectron -- "$URL"
