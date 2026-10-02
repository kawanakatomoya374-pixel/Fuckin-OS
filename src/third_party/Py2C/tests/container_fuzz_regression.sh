#!/bin/sh
set -eu

CC=${CC:-cc}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
CASES=${P2C_FUZZ_CASES:-48}
SEEDS=${P2C_FUZZ_SEEDS:-"12648430 24237 17412"}
ROOT_OUT="$ROOT/build/tests/container_fuzz"

run_one() {
    seed=$1
    cases=$2
    outdir=$3
    mkdir -p "$outdir"
    source="$outdir/source.py"
    cfile="$outdir/source.c"
    pyout="$outdir/python.out"
    cout="$outdir/c.out"
    bin="$outdir/program"

    python3 "$ROOT/tests/generate_container_fuzz.py" --seed "$seed" --cases "$cases" --output "$source" >/dev/null
    python3 "$source" > "$pyout"
    "$ROOT/build/python-code-to-c" "$source" -o "$cfile"
    $CC -I"$ROOT/include" -std=gnu11 -O2 -Wall -Wextra -Werror \
        "$cfile" \
        "$ROOT/src/runtime/python_code_to_c_runtime.c" \
        "$ROOT/src/common/python_code_to_c_common.c" \
        "$ROOT/src/platform/python_code_to_c_platform.c" \
        "$ROOT/src/platform/python_code_to_c_platform_hosted.c" \
        "$ROOT/src/platform/python_code_to_c_gui.c" \
        "$ROOT/src/modules/python_code_to_c_pygame.c" \
        -lm -o "$bin"
    "$bin" > "$cout"
    diff -u "$pyout" "$cout"
}

minimize_failure() {
    seed=$1
    high=$2
    low=1
    while [ "$low" -lt "$high" ]; do
        mid=$(( (low + high) / 2 ))
        if run_one "$seed" "$mid" "$ROOT_OUT/minimize-$seed-$mid" >/dev/null 2>&1; then
            low=$((mid + 1))
        else
            high=$mid
        fi
    done
    repro="$ROOT_OUT/repro-seed-$seed-cases-$low"
    rm -rf "$repro"
    run_one "$seed" "$low" "$repro" >/dev/null 2>&1 || true
    printf '%s\n' "container_fuzz_failure: seed=$seed minimal_cases=$low reproduction=$repro" >&2
    printf '%s\n' "reproduce: CC=$CC P2C_FUZZ_SEEDS=$seed P2C_FUZZ_CASES=$low sh tests/container_fuzz_regression.sh" >&2
}

if [ "$CASES" -lt 1 ]; then
    printf '%s\n' 'P2C_FUZZ_CASES must be positive' >&2
    exit 2
fi

rm -rf "$ROOT_OUT"
mkdir -p "$ROOT_OUT"
seed_count=0
for seed in $SEEDS; do
    seed_count=$((seed_count + 1))
    outdir="$ROOT_OUT/seed-$seed"
    if ! run_one "$seed" "$CASES" "$outdir"; then
        minimize_failure "$seed" "$CASES"
        exit 1
    fi
    printf '%s\n' "container_fuzz_seed_ok: seed=$seed cases=$CASES"
done
printf '%s\n' "container_fuzz_ok: generator=container-fuzz-v1 seeds=$seed_count cases_per_seed=$CASES total_operations=$((seed_count * CASES))"
