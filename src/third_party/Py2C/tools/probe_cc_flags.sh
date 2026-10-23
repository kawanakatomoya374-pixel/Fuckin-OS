#!/bin/sh
# probe_cc_flags.sh CC FLAG...
#
# CC が受け付けるフラグだけを空白区切りで標準出力へ書く。
#
# 目的: GCC 13 には無く GCC 14 で追加された警告（例: -Wcalloc-transposed-args）を
# 含む厳格警告の集合を、古いコンパイラでも「黙って落ちない」形で使うため。
# 受け付けないフラグは、コンパイラ自身のエラーメッセージ（"unrecognized command-line
# option" / "unknown warning option"）から特定して取り除く。全フラグを1回のコンパイルで
# 検査するので、Makefile が再帰的に呼ばれても起動コストは小さい。
#
# 注意: 落としたフラグは呼び出し側（Makefile）が make 起動時に一覧表示する。
#       ここでは -Werror を付けて検査するため、clang の「未知の警告」も確実に検出できる。
set -u

cc=${1:?usage: probe_cc_flags.sh CC FLAG...}
shift

tmp=$(mktemp -d 2>/dev/null) || tmp=${TMPDIR:-/tmp}/p2c_probe.$$
mkdir -p "$tmp" || exit 1
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

printf 'int main(void) { return 0; }\n' > "$tmp/probe.c"

flags="$*"
round=0
while :; do
    round=$((round + 1))
    if [ "$round" -gt 64 ]; then
        break
    fi
    # shellcheck disable=SC2086
    if err=$("$cc" -Werror -c "$tmp/probe.c" -o "$tmp/probe.o" $flags 2>&1); then
        break
    fi
    bad=$(printf '%s\n' "$err" | sed -n \
        -e "s/.*unrecognized command-line option '\\([^']*\\)'.*/\\1/p" \
        -e "s/.*unknown warning option '\\([^']*\\)'.*/\\1/p" \
        | sort -u)
    if [ -z "$bad" ]; then
        # 既知の形式で説明できないエラー（フラグ以外の問題）。残りは本コンパイルで報告させる。
        break
    fi
    kept=""
    for f in $flags; do
        drop=0
        for b in $bad; do
            if [ "$f" = "$b" ]; then
                drop=1
                break
            fi
        done
        if [ "$drop" -eq 0 ]; then
            kept="$kept $f"
        fi
    done
    # 何も落とせなかった（フラグ以外が原因）、または全部落ちた場合は打ち切る
    if [ "$kept" = " $flags" ] || [ -z "$kept" ]; then
        flags=$kept
        break
    fi
    flags=$kept
done

# 先頭の空白を除いて出力する
printf '%s\n' "$flags" | sed 's/^ *//'
