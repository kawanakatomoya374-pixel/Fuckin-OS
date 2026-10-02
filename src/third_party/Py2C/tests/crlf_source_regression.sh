#!/bin/sh
# 改行コードの回帰: CRLF / CR / LF のどのソースでも生成Cが完全に一致すること。
#
# Pythonはソースの改行コードを問わず同じ意味を持つ（universal newlines）。
# 以前はこの正規化が無く、Windowsで保存したCRLFソースでは行末のCRが未知
# トークンとなってインデント計算とclass本体のメソッド検出が壊れ、
# 「メソッドがクラスに登録されない（実行時にAttributeError）」という
# 無言の誤変換を起こしていた。本スクリプトはその再発を防ぐ品質ゲートである。
set -eu

P2C_COMPILER=${P2C_COMPILER:-./build/python-code-to-c}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p build/tests

SRC=tests/crlf_source_probe.py
LF=build/tests/crlf_probe_lf.py
CRLF=build/tests/crlf_probe_crlf.py
CR=build/tests/crlf_probe_cr.py
OUT_LF=build/tests/crlf_probe_lf.c
OUT_CRLF=build/tests/crlf_probe_crlf.c
OUT_CR=build/tests/crlf_probe_cr.c

python3 - "$SRC" "$LF" "$CRLF" "$CR" <<'PY'
import sys
src, lf, crlf, cr = sys.argv[1:5]
data = open(src, "rb").read().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
open(lf, "wb").write(data)
open(crlf, "wb").write(data.replace(b"\n", b"\r\n"))
open(cr, "wb").write(data.replace(b"\n", b"\r"))
PY

"$P2C_COMPILER" "$LF" -o "$OUT_LF"
"$P2C_COMPILER" "$CRLF" -o "$OUT_CRLF"
"$P2C_COMPILER" "$CR" -o "$OUT_CR"

if ! diff -u "$OUT_LF" "$OUT_CRLF"; then
    printf '%s\n' 'crlf_regression: CRLF source produced different C output' >&2
    exit 1
fi
if ! diff -u "$OUT_LF" "$OUT_CR"; then
    printf '%s\n' 'crlf_regression: CR source produced different C output' >&2
    exit 1
fi

# 生成Cにクラスメソッドがクラス所属として出ていること（誤変換の直接検出）
if ! grep -q 'Probe__method' "$OUT_CRLF"; then
    printf '%s\n' 'crlf_regression: class methods were not attached to the class in CRLF input' >&2
    exit 1
fi

printf '%s\n' 'crlf_regression_ok: LF/CRLF/CR produce identical C'
