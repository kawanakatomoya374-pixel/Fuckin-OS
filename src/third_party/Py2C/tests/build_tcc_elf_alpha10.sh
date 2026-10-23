#!/bin/sh
# TinyCC だけで ELF を作り、tools/p2c_elfcheck（binutils 非依存）で検査する。
#
#   sh tests/build_tcc_elf_alpha10.sh full     CLI の ELF（TinyCC 既定の動的リンク）
#   sh tests/build_tcc_elf_alpha10.sh hobbyos  HobbyOS 向け ELF（-static -nostdlib、libc なし）
#
# 入力は環境変数で受け取る（Makefile の tcc-elf / tcc-hobbyos-elf から呼ばれる）:
#   TCC                 使う TinyCC（既定 tcc）
#   P2C_ELFCHECK        p2c_elfcheck の実行ファイル（TinyCC でビルド済みであること）
#   P2C_OBJS            full: リンクする .o の並び（Makefile の TCC_OBJFILES）
#   ELF_BUILD           full: 出力ディレクトリ
#   HOBBYOS_ELF_BUILD   hobbyos: 出力ディレクトリ
#   PROGRAM_C           hobbyos: --embed-entry で変換した program.c
#
# 注意（TinyCC の制約。ここで隠さず記録する）:
#   - TinyCC は -static と glibc の CRT を組み合わせられないため、CLI の ELF は動的リンクになる。
#   - TinyCC はリンカスクリプト（ENTRY / -T）と --entry / --defsym を受け付けない。HobbyOS の
#     入口は _start（templates/hobby_os/embed/hobby_os_tcc_start.c のシム）になる。
#   - シンボル表を残すため -g を付ける（無いと未定義シンボルを検査できない）。ロード対象には含まれない。
set -eu

VARIANT=${1:-full}
TCC=${TCC:-tcc}
CHECK=${P2C_ELFCHECK:?P2C_ELFCHECK must name the p2c_elfcheck tool}

if [ ! -x "$CHECK" ]; then
    echo "build_tcc_elf: p2c_elfcheck not built: $CHECK (run: make elfcheck CC=tcc)" >&2
    exit 1
fi

# 取り違え防止: 指定されたコンパイラが本当に TinyCC であることを確かめる
case "$($TCC --version 2>/dev/null | head -n 1)" in
    *"tcc version"*) ;;
    *) echo "build_tcc_elf: TCC=$TCC is not TinyCC" >&2; exit 1 ;;
esac

# TinyCC のランタイム補助（__va_arg / __floatundidf / __fixunsdfdi 等）は libtcc1.a にある。
# -nostdlib では自動リンクされないため、HobbyOS 向けは明示的に渡す。場所は TinyCC 自身に
# 問い合わせる（-print-search-dirs の install:）。LIBTCC1 で上書きもできる。
find_libtcc1() {
    if [ -n "${LIBTCC1:-}" ]; then
        printf '%s\n' "$LIBTCC1"
        return 0
    fi
    install_dir=$("$TCC" -print-search-dirs 2>/dev/null | sed -n 's/^install: *//p' | head -n 1)
    if [ -n "$install_dir" ] && [ -f "$install_dir/libtcc1.a" ]; then
        printf '%s\n' "$install_dir/libtcc1.a"
        return 0
    fi
    return 1
}

# $1 = 対象 ELF、以降 = p2c_elfcheck の検査オプション。マニフェストを残し、違反なら失敗する。
write_manifest() {
    bin=$1
    shift
    manifest="${bin%.elf}.elf.txt"   # GCC 経路と同じ名前（x.elf.txt）にそろえる
    if "$CHECK" "$@" "$bin" > "$manifest"; then
        rc=0
    else
        rc=$?
    fi
    cat "$manifest"
    return $rc
}

case "$VARIANT" in
full)
    out_dir=${ELF_BUILD:-build/elf-tcc}
    mkdir -p "$out_dir"
    out="$out_dir/python-code-to-c.elf"
    echo "=== tcc: CLI ELF（TinyCC の既定どおり動的リンク） ==="
    # shellcheck disable=SC2086
    "$TCC" -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Werror -Wunsupported -Wwrite-strings \
        ${P2C_OBJS:?P2C_OBJS is required for the full variant} -lm -o "$out"
    cp -f "$out" "$out_dir/python-code-to-c"
    # 動的リンク（PT_INTERP あり）は想定どおり。ELF 形式と W^X だけを検査する。
    write_manifest "$out" --exec
    "$out" --supported | head -n 1
    echo "tcc_elf_full_ok: $out (dynamic: TinyCC cannot link glibc statically; compat copy: $out_dir/python-code-to-c)"
    ;;

hobbyos)
    out_dir=${HOBBYOS_ELF_BUILD:-build/hobbyos-tcc}
    program=${PROGRAM_C:?PROGRAM_C must name the generated program.c}
    mkdir -p "$out_dir"
    out="$out_dir/python-code-to-c-hobbyos.elf"
    echo "=== tcc: HobbyOS ELF（-static -nostdlib、libc なし） ==="
    # HobbyOS の契約: libc は使わない。malloc は p2c_embed（LIBC_HEAP）、setjmp/longjmp は
    # 参照実装（examples/embed/x86_64_setjmp.c）が提供する（PYTHON_CODE_TO_C_NO_LIBC_STUBS）。
    # ENTRY は _start のシム（hobby_os_tcc_start.c）→ p2c_hobbyos_entry()。
    libtcc1=$(find_libtcc1) || {
        echo "build_tcc_elf: libtcc1.a not found (set LIBTCC1=/path/to/libtcc1.a)" >&2
        exit 1
    }
    "$TCC" -std=c99 -O2 -g -Wall -Werror -Wwrite-strings \
        -ffreestanding -fno-builtin -fno-stack-protector \
        -I./include -I./templates/hobby_os -I./examples/embed \
        -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
        -DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT \
        -nostdlib -static \
        "$program" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_embed.c \
        templates/hobby_os/embed/hobby_os_entry.c \
        templates/hobby_os/embed/hobby_os_libc.c \
        examples/embed/x86_64_setjmp.c \
        templates/hobby_os/embed/hobby_os_tcc_start.c \
        "$libtcc1" \
        -o "$out"
    # 静的（PT_INTERP / PT_DYNAMIC なし）・未定義の大域シンボル無し・W^X・入口は _start
    write_manifest "$out" --exec --static --no-undef --entry-sym _start
    echo "tcc_elf_hobbyos_ok: $out (entry=_start shim -> p2c_hobbyos_entry)"
    ;;

*)
    echo "unknown variant: $VARIANT (use full or hobbyos)" >&2
    exit 2
    ;;
esac
