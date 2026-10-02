#!/bin/bash
# make help に出る全てのターゲットを順に実行し、結果をまとめる。
# 使い方: bash tests/run_all_make_targets.sh [timeout秒]
set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
LIMIT=${1:-1200}
OUT=/tmp/helprun
rm -rf "$OUT"; mkdir -p "$OUT"

# make help から「make <...>」行を抽出し、実行コマンド列に変換する。
mapfile -t CMDS < <(make help | sed -n 's/^  make \([^ ].*\)$/\1/p' | sed 's/  \+.*$//' | awk '{
    n = split($0, parts, /[|]/);
    for (i = 1; i <= n; i++) {
        gsub(/^ +| +$/, "", parts[i]);
        if (parts[i] != "") print parts[i];
    }
}')

: > "$OUT/summary.txt"
printf 'targets: %d\n' "${#CMDS[@]}" | tee -a "$OUT/summary.txt"

i=0
for cmd in "${CMDS[@]}"; do
    i=$((i + 1))
    safe=$(printf '%s' "$cmd" | tr ' /|' '___')
    log="$OUT/$(printf '%02d' "$i")_${safe}.log"
    # make run は INPUT が必要。freestanding は cross-cc のプレースホルダを外す。
    case "$cmd" in
        run|"run INPUT="*|"run INPUT"*)
            full="make run INPUT=tests/complex_alpha06.py" ;;
        "freestanding CC="*|"freestanding"*"<cross-cc>"*)
            full="make freestanding" ;;
        "run-gui")
            full="timeout 8 make run-gui" ;;
        *)
            full="make $cmd" ;;
    esac
    start=$(date +%s)
    timeout "$LIMIT" bash -c "$full" > "$log" 2>&1
    rc=$?
    dur=$(( $(date +%s) - start ))
    if [ $rc -eq 0 ]; then
        res=PASS
    elif [ $rc -eq 124 ]; then
        res=TIMEOUT
    else
        res="FAIL(rc=$rc)"
    fi
    printf '%-14s %-22s %5ss\n' "$res" "make $cmd" "$dur" | tee -a "$OUT/summary.txt"
done
printf '%s\n' '--- done ---' | tee -a "$OUT/summary.txt"
