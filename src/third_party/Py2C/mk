#!/bin/sh
# mk - make をもっと手軽に呼ぶための薄いラッパー
#
#   mk          ... make            と同じ
#   mk 8        ... make -j8        と同じ（数字1つだけを指定すると並列ジョブ数になる）
#   mk n        ... make -j$(nproc) と同じ（"n" で使えるコア数をすべて使う）
#   mk test     ... make test       と同じ（数字/n 以外はそのまま make のターゲット/引数として渡す）
#   mk 8 smoke  ... make -j8 smoke  と同じ（並列ジョブ数のあとにターゲットを続けてもよい）
#   mk n smoke  ... make -j$(nproc) smoke と同じ
#
# 使えるターゲットは `mk help`（= make help）を参照してください。
set -eu

nproc_count() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v getconf >/dev/null 2>&1 && getconf _NPROCESSORS_ONLN >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}

if [ $# -eq 0 ]; then
    exec make
fi

case "$1" in
    n)
        n=$(nproc_count)
        shift
        exec make -j"$n" "$@"
        ;;
    ''|*[!0-9]*)
        exec make "$@"
        ;;
    *)
        n="$1"
        shift
        exec make -j"$n" "$@"
        ;;
esac
