#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
# 厳格プロファイルを順に実行し、段ごとの結果を build/strict/ に集約する。
# バグ狩り用: 途中で失敗しても続行して全結果を出す（CIの合否は個別ターゲット
# make test-asan-strict 等で見る）。各段の詳細は build/strict/<name>.log。
#
#   使い方: sh tests/strict_batch_alpha10.sh [stage ...]
set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

OUT=$BD/strict
mkdir -p "$OUT"
: > "$OUT/summary.txt"

run_stage() {
    name=$1
    shift
    start=$(date +%s)
    if "$@" > "$OUT/$name.log" 2>&1; then
        dur=$(( $(date +%s) - start ))
        printf 'PASS  %-24s (%ss)\n' "$name" "$dur" | tee -a "$OUT/summary.txt"
    else
        dur=$(( $(date +%s) - start ))
        printf 'FAIL  %-24s (%ss)\n' "$name" "$dur" | tee -a "$OUT/summary.txt"
    fi
}

if [ "$#" -gt 0 ]; then
    for want in "$@"; do
        case "$want" in
            conformance) run_stage 00-conformance sh tests/conformance_regression.sh ;;
            probe) run_stage 01-probe sh tests/semantic_probe_alpha10.sh ;;
            asan-strict) run_stage 02-asan-strict sh tests/strict_dynamic_alpha10.sh asan-strict both ;;
            asan-strict-probe) run_stage 02a-asan-probe sh tests/strict_dynamic_alpha10.sh asan-strict probe ;;
            asan-uar-probe) run_stage 02b-asan-uar-probe env ASAN_UAR=1 sh tests/strict_dynamic_alpha10.sh asan-strict probe ;;
            asan-pairs-probe) run_stage 02c-asan-pairs-probe env ASAN_PTR_PAIRS=1 sh tests/strict_dynamic_alpha10.sh asan-strict probe ;;
            ubsan-deep) run_stage 03-ubsan-deep sh tests/strict_dynamic_alpha10.sh ubsan-deep both ;;
            clang-int) run_stage 04-clang-integer env CC=clang sh tests/strict_dynamic_alpha10.sh clang-int both ;;
            msan) run_stage 05-msan env CC=clang sh tests/strict_dynamic_alpha10.sh msan both ;;
            harden) run_stage 06-harden-generated sh tests/strict_dynamic_alpha10.sh harden both ;;
            opt3) run_stage 06b-opt3 sh tests/strict_dynamic_alpha10.sh opt3 both ;;
            lsan) run_stage 06c-lsan sh tests/strict_dynamic_alpha10.sh lsan both ;;
            warn-clang) run_stage 07-warn-clang sh -c 'make test-warn-clang' ;;
            hardened-core) run_stage 08-hardened-core sh -c 'make test-hardened-core' ;;
            clang-build) run_stage 09-clang-build sh tests/clang_build_alpha10.sh ;;
            avinit) run_stage 10-avinit-differential sh -c 'AVINIT_RUN=${AVINIT_RUN:-1} make test-avinit-differential' ;;
            *) printf 'unknown stage: %s\n' "$want" >&2 ;;
        esac
    done
else
    run_stage 00-conformance sh tests/conformance_regression.sh
    run_stage 01-probe sh tests/semantic_probe_alpha10.sh
    run_stage 02-asan-strict sh tests/strict_dynamic_alpha10.sh asan-strict
    run_stage 03-ubsan-deep sh tests/strict_dynamic_alpha10.sh ubsan-deep
    run_stage 04-clang-integer env CC=clang sh tests/strict_dynamic_alpha10.sh clang-int
    run_stage 05-msan env CC=clang sh tests/strict_dynamic_alpha10.sh msan
    run_stage 06-harden-generated sh tests/strict_dynamic_alpha10.sh harden
    run_stage 07-hardened-core sh -c 'make test-hardened-core'
    run_stage 08-avinit-differential sh -c 'make test-avinit-differential'
fi

grep -q '^FAIL' "$OUT/summary.txt" && exit 1
exit 0
