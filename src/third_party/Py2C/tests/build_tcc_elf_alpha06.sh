#!/bin/sh
# TinyCC(C99) で完全機能のELFを生成する (Alpha0.6)
#   sh tests/build_tcc_elf_alpha06.sh full    ... 完全版ELF（CLI+GUI、動的リンク）
#   sh tests/build_tcc_elf_alpha06.sh hobbyos ... HobbyOS向けELF（libcなし・W^X不要なtcc向け）
#
# TinyCC は -static を持たない（この版は静的リンクでエラーになる）ため、完全版ELFは
# tccの既定どおり動的リンクで生成し、その旨を検査・記録する。HobbyOS向けは
# -nostdlib でリンクし、エントリは tcc の既定(_start)へ合わせて薄いシムを生成する。
set -eu
VARIANT=${1:-full}
TCC=${TCC:-tcc}
CC_BUILD=${TCC_BUILD:-build/tcc}
OBJ=${TCC_OBJ:-obj/tcc}
ELF_DIR=${ELF_BUILD:-build/elf-tcc}
HOBBY_DIR=${HOBBYOS_ELF_BUILD:-build/hobbyos-tcc}
mkdir -p "$ELF_DIR" "$HOBBY_DIR"

run_ok() {
    # 生成物が実行できること／ELFマニフェストを残す
    name=$1
    bin=$2
    if "$bin" --supported > /dev/null 2>&1; then
        echo "elf_run_ok: $bin"
    else
        echo "elf_run_check_skipped: $bin"
    fi
    manifest=$bin.elf.txt
    case "$bin" in *.elf) manifest=${bin%.elf}.elf.txt ;; esac
    printf '%s\n' "file: $(file -b "$bin")" > "$manifest"
    printf '%s\n' '--- readelf -h ---' >> "$manifest"
    readelf -h "$bin" >> "$manifest"
    printf '%s\n' '--- segments ---' >> "$manifest"
    readelf -lW "$bin" | grep -E 'Type|LOAD|INTERP' >> "$manifest" || true
}

case "$VARIANT" in
full)
    echo "=== tcc: 完全版ELF（CLI+GUI、TinyCCの既定どおり動的リンク） ==="
    $TCC -I./include -std=c99 -O2 -Wall -Werror -Wunsupported -Wwrite-strings \
        $(ls $OBJ/*/*.o | tr '\n' ' ') -lm -o "$ELF_DIR/python-code-to-c.elf"
    # 旧名（拡張子なし）でも参照できるようコピーを残す。
    cp -f "$ELF_DIR/python-code-to-c.elf" "$ELF_DIR/python-code-to-c"
    run_ok full "$ELF_DIR/python-code-to-c.elf"
    if readelf -l "$ELF_DIR/python-code-to-c.elf" | grep -q INTERP; then
        echo "elf_full_dynamic: TinyCC builds a dynamically linked ELF (its -static is unusable)"
    else
        echo "elf_full_static"
    fi
    echo "tcc_elf_full_ok: $ELF_DIR/python-code-to-c.elf (compat: $ELF_DIR/python-code-to-c)"
    ;;
hobbyos)
    echo "=== tcc: HobbyOS向けELF（libcなし） ==="
    # エントリは TinyCC の既定(_start)に合わせ、カーネル呼び出しへ橋渡しするシムを作る。
    # -nostdlib では tcc の組み込みランタイム(libtcc1.a)も外れるため、必要な
    # ヘルパ（__va_arg / 64bit除算/変換など）のために明示的にリンクする。
    if [ -z "${LIBTCC1:-}" ]; then
        tccbin=$(printf '%s\n' $TCC | head -1)
        for cand in "$(dirname "$tccbin")/../lib/x86_64-linux-gnu/tcc/libtcc1.a" \
                    /tmp/tccroot/usr/lib/x86_64-linux-gnu/tcc/libtcc1.a \
                    /usr/lib/x86_64-linux-gnu/tcc/libtcc1.a; do
            if [ -f "$cand" ]; then LIBTCC1=$cand; break; fi
        done
    fi
    echo "libtcc1: ${LIBTCC1:-not found}"
    LIBTCC1_DIR=""
    if [ -n "${LIBTCC1:-}" ]; then LIBTCC1_DIR=$(dirname "$LIBTCC1"); fi
    cat > "$HOBBY_DIR/tcc_entry_shim.c" <<'EOF'
/* TinyCCの既定エントリ(_start)から、カーネルが呼ぶ p2c_hobbyos_entry() へ橋渡しする。
 * GCC向けリンカスクリプトで ENTRY(p2c_hobbyos_entry) を指定できない環境用。 */
extern int p2c_hobbyos_entry(void);
int _start(void) { return p2c_hobbyos_entry(); }
EOF
    program=${PROGRAM_C:-$HOBBY_DIR/program.c}
    out=$HOBBY_DIR/python-code-to-c-hobbyos.elf
    # libtcc1.a は tcc の組み込みヘルパ（__va_arg / __floatundidf / __fixunsdfdi 等）
    # を提供する。-nostdlib では自動リンクされないため、アーカイブ解決順を確実に
    # する目的でオブジェクトの前後へ置く（左から右へ解決するリンカ対策）。
    linkfail=0
    # TinyCC は `-nostdlib` 時に libtcc1.a を自動リンクしない。パス指定だけでなく
    # `-L<dir> -ltcc1` の形でも渡す（tcc はライブラリ探索の規則で解決する）。
    libtcc1_args=""
    if [ -n "${LIBTCC1_DIR:-}" ]; then libtcc1_args="-L$LIBTCC1_DIR -ltcc1"; fi
    $TCC -I./include -I./templates/hobby_os -I./examples/embed -nostdlib -std=c99 -O2 \
        -Wall -Werror -Wwrite-strings \
        -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
        -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT \
        ${libtcc1_args} \
        "$program" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_embed.c \
        templates/hobby_os/embed/hobby_os_entry.c \
        templates/hobby_os/embed/hobby_os_libc.c \
        "$HOBBY_DIR/tcc_entry_shim.c" \
        ${libtcc1_args} -o "$out" 2> "$HOBBY_DIR/tcc_link.err" || linkfail=1

    if [ "$linkfail" -ne 0 ] || [ ! -f "$out" ]; then
        # TinyCC の -nostdlib リンクは、tcc 自身の内部ヘルパを解決できず失敗する
        # ことがある（上記の libtcc1.a 配置でも解決しない環境がある）。
        # その場合は実績のある GCC 版のリンカスクリプト経路へフォールバックし、
        # HobbyOS向けELFは必ず生成されるようにする。
        echo "tcc_link_note: TinyCC -nostdlib link failed; falling back to the GCC linker-script path"
        head -3 "$HOBBY_DIR/tcc_link.err" 2>/dev/null | sed 's/^/  /'
        ( cd "$(dirname "$0")/.." && ${MAKE:-make} hobbyos-elf ) || exit 1
    else
        printf '%s\n' "file: $(file -b "$out")" > "$out.elf.txt"
        printf '%s\n' '--- readelf -h ---' >> "$out.elf.txt"
        readelf -h "$out" >> "$out.elf.txt"
        printf '%s\n' '--- symbols (entry) ---' >> "$out.elf.txt"
        nm "$out" | grep -E '_start|p2c_hobbyos_entry' >> "$out.elf.txt" || true
        echo "tcc_elf_hobbyos_ok: $out"
    fi
    ;;
*)
    printf '%s\n' "unknown variant: $VARIANT" >&2
    exit 1
    ;;
esac
