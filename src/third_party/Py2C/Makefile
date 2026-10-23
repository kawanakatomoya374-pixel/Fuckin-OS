# Python Code to C Beta0.1 (Py2C B0.1)
# Hosted build: make
# Freestanding build: make freestanding CC=<cross-cc> AR=<cross-ar>
# TinyCC only build:  make tcc-full   (no GCC/Clang/binutils needed)
PROJECT := python-code-to-c
VERSION := 0.1.0
CC ?= cc
AR ?= ar
PYTHON ?= python3
export CC

# --- コンパイラ種別の判定（gcc / clang / tcc） -------------------------------------
# 種別ごとに既定の警告・-MMD・リンカ・AR・BUILD/OBJ を切り替える。TinyCC は GCC 専用の
# フラグを黙って無視するもの（-fsanitize / -fanalyzer / -Wstack-usage）と、拒否するもの
# （-MMD / -z,relro / --wrap / -no-pie）があるため、tcc のときは専用の既定値を使う。
CC_VERSION_LINE := $(shell $(CC) --version 2>/dev/null | head -n 1)
ifneq ($(findstring tcc version,$(CC_VERSION_LINE)),)
CC_KIND := tcc
else ifneq ($(findstring clang,$(CC_VERSION_LINE)),)
CC_KIND := clang
else
CC_KIND := gcc
endif
IS_TCC := $(if $(filter tcc,$(CC_KIND)),1,)
ifeq ($(IS_TCC),1)
# TinyCC は `tcc -ar` でアーカイブを作れるため、binutils の ar を要求しない。
ifeq ($(AR),ar)
AR := $(CC) -ar
endif
endif
INPUT ?= examples/fib.py
FREESTANDING_ARCH_CFLAGS ?=
FREESTANDING_EXTRA_CFLAGS ?=
P2C_FUZZ_CASES ?= 48
P2C_FUZZ_SEEDS ?= 12648430 24237 17412
# コンパイラ本体とfreestandingコアが共有する厳格な警告基準。
#   -Wconversion / -Wsign-conversion ... 暗黙の切り詰めと符号反転
#   -Wcast-qual / -Wwrite-strings   ... const契約の破壊
#   -Wvla                           ... 組込み向けに可変長スタック配列を排除
#   -Wcast-align=strict             ... アラインメントを上げるキャスト（組込みで致命的）
#   -Wlogical-op / -Wduplicated-cond / -Wduplicated-branches ... 条件式の書き間違い
#   -Wstrict-overflow=2 / -Wshift-overflow=2 ... 符号付きオーバーフロー前提の最適化
#   -Wformat-overflow=2 / -Wformat-truncation=2 / -Wstringop-overflow=4 ...
#       snprintf等の切り詰め・領域外書き込み
#   -Wuse-after-free=3 / -Warray-bounds=2 ... 解放後利用・配列境界
#   -Wjump-misses-init / -Wnested-externs / -Wmissing-declarations / -Wredundant-decls
#       ... 宣言と初期化の取り違え、スコープの逸脱
#   -Wswitch-default / -Wimplicit-fallthrough=5 ... switchの網羅とfall-through
#   -Wunused-macros / -Wmissing-parameter-type / -Wold-style-definition ...
#       使われない定義・暗黙の古い書き方
# 意図的に有効化しないフラグ:
#   -Wswitch-enum: ASTノード種別のswitchはdefault:節で未対応種別を診断する設計で
#     あり、全case列挙(1200件超)に見合う安全性を生まない。網羅性はtests/の
#     CPython差分回帰で担保する。
#   -Wdeclaration-after-statement: C11の混合宣言は本コードベースの意図した書き方で
#     あり、C89配置への書き換えは646箇所の移動（初期化式の評価順も変わりうる）に
#     なる割に安全性を増やさない。オブジェクトの寿命とGCの可視性は、Predeclare
#     パスとテストで担保する。
#   -Wc++-compat: C++互換は対象外（C11専用）。void*からの暗黙変換だけで108件出る。
#   -Wnull-dereference: TU毎の通常ビルドでは0件だが、単一ヘッダ構成では
#     全ソースが1TUになりGCCの関数間解析が効くため、未チェック確保と誤検出が
#     混在した約300件の "potential null pointer dereference" を報告する。
#     有効化には全域のNULL契約監査が前提となるため、別途監査タスクとして扱う。
# 警告基準（Round-5で拡張）。各項目は「実バグを捕まえる」ことを確認したうえで
# 採用しており、ノイズ源になるものは理由付きで除外している。
#   -Wclobbered: setjmp/longjmp をまたいで変更された自動変数（＝longjmp後に不定値に
#     なり得る変数）を指摘する。本ランタイムは例外とジェネレータで P2C_SETJMP を
#     多用するため最優先で有効化した（実際に生成C側の潜在不具合を1件検出し、
#     volatile 付与で修正済み）。
#   -Wformat-security: 非リテラル書式（printf(fmt) 形式）の脆弱性を指摘。
#   -Wbidi-chars=any,ucn: 双方向制御文字（Trojan Source）を検出。ソースコードを
#     入力として扱うツールなので、表示と実体の食い違いを許さない。
#   -Wtrampolines: 実行可能スタックを要するトランポリン生成を禁止（組込み/自作OS）。
#   -Wstrict-aliasing=2: union/キャストを多用するランタイムで型破り参照を検査。
#   -Wpointer-to-int-cast / -Wint-to-pointer-cast: 32bit/64bit 取り違えの検出。
#   -Wzero-length-bounds / -Wflex-array-member-not-at-end / -Wstringop-overread /
#     -Warray-compare / -Wsizeof-pointer-div / -Wsizeof-array-div /
#     -Wmemset-transposed-args / -Wmemset-elt-size / -Wnonnull-compare:
#     境界・サイズ・引数順に関する古典的な取り違えを静的に検出。
#   -Wtautological-compare / -Winit-self / -Wshift-negative-value /
#     -Wshift-count-overflow / -Wlogical-not-parentheses / -Wabsolute-value /
#     -Wenum-conversion: 常真比較・自己初期化・未定義シフト・符号なしabs など
#     「書いたつもりの意味と違う」式を検出。
#   -Wvla-parameter / -Woverlength-strings / -Wnormalized=nfkc: 移植性（可変長配列
#     引数・C90の翻訳限界・識別子正規化）を担保。
#   -fno-common: 一時定義のマージを禁止し、シンボル定義の重複をリンク時に検出する
#     （GCC 10以降の既定だが、明示して環境差をなくす）。
#
# STACK_USAGE_BUDGET は組込み向けのスタック予算（後述）。
#   -Wstack-usage=$(STACK_USAGE_BUDGET): 組込み/自作OS向けにスタック使用量の上限を
#     ビルド時に強制する。既定 16384 は全ソース（ホスト側ツールを含む）を対象とした
#     安全側の上限で、実測の最大はツールの 8304 バイト・組込みコアの 3456 バイト。
#     組込みコアだけを厳しく締めたい場合は STACK_USAGE_BUDGET を下げるか、
#     make test-stack-budget（コアを 4096 でビルド）を使う。
# 不採用（理由付き）:
#   -Wc++-compat       : C++互換は対象外（void*からの暗黙変換だけで108件）。
#   -Wnull-dereference : 単一ヘッダ構成で全域解析になり約300件の誤検出混在。
#                        全域のNULL契約監査が前提のため別タスク扱い。
#   -Wuseless-cast     : 「型の明示」として意図的に書いたキャストを41件指摘する。
#                        冗長キャストは可読性のための記述であり、実バグ検出に寄与しない。
#   -Wdeclaration-after-statement: C11では宣言と文の混在は合法。C89スタイルを強制する
#                        この警告は言語仕様に反する制約になるため不採用。
#   -Wsuggest-attribute / -Wunsafe-loop-optimizations: 性能ヒントであり正しさの
#                        検査ではない。make test-opt-hints に分離した。
STACK_USAGE_BUDGET ?= 16384
WARN_CFLAGS_WISH ?= -Wall -Wextra -Werror -Wpedantic \
	-Wshadow -Wformat=2 -Wformat-security -Wno-format-nonliteral \
	-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition \
	-Wredundant-decls -Wundef \
	-Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings \
	-Wdouble-promotion -Wvla -Wvla-parameter -Wfloat-equal \
	-Wcast-align=strict -Wpointer-arith -Wbad-function-cast -Wnested-externs \
	-Wjump-misses-init -Wlogical-op -Wduplicated-cond -Wduplicated-branches \
	-Wrestrict -Wshift-overflow=2 -Wformat-overflow=2 -Wformat-truncation=2 \
	-Wformat-signedness -Wstringop-overflow=4 -Wstringop-truncation \
	-Warray-bounds=2 -Wmissing-declarations -Warith-conversion \
	-Wmultistatement-macros -Wsizeof-pointer-memaccess -Wsizeof-array-argument \
	-Wuse-after-free=3 -Wunused-macros -Wswitch-default -Wcast-function-type \
	-Wimplicit-fallthrough=5 -Wmissing-parameter-type -Wcalloc-transposed-args \
	-Wstrict-overflow=2 -Wstrict-aliasing=2 -Wno-error=strict-aliasing \
	-Wclobbered -Wtautological-compare -Winit-self -Wshift-negative-value \
	-Wshift-count-overflow -Wlogical-not-parentheses -Wabsolute-value \
	-Wenum-conversion -Wzero-length-bounds -Wflex-array-member-not-at-end \
	-Wstringop-overread -Warray-compare -Wsizeof-pointer-div -Wsizeof-array-div \
	-Wmemset-transposed-args -Wmemset-elt-size -Wnonnull-compare \
	-Wpointer-to-int-cast -Wint-to-pointer-cast \
	-Wtrampolines -Wbidi-chars=any,ucn -Woverlength-strings -Wnormalized=nfkc \
	-Wstack-usage=$(STACK_USAGE_BUDGET) -fno-common
# 使用中のコンパイラが受け付ける警告だけを WARN_CFLAGS に残す（1回のコンパイルで検査）。
# 例: GCC 13 は -Wcalloc-transposed-args（GCC 14 以降）を知らないので、その1件だけを省く。
# 省いたフラグは make 起動時（MAKELEVEL=0）に必ず表示する。黙って落とさない。
ifeq ($(IS_TCC),1)
WARN_CFLAGS ?= -Wall -Wextra -Werror -Wwrite-strings -Wunsupported
else ifndef WARN_CFLAGS
WARN_CFLAGS := $(shell sh tools/probe_cc_flags.sh "$(CC)" $(WARN_CFLAGS_WISH))
WARN_DROPPED := $(filter-out $(WARN_CFLAGS),$(WARN_CFLAGS_WISH))
ifeq ($(MAKELEVEL),0)
ifneq ($(WARN_DROPPED),)
$(info note: $(CC) does not accept these warning flags and they are omitted: $(WARN_DROPPED))
endif
endif
endif
# ホスト環境でのみ意味のある実行時ハードニング（開発機・CIでの検証用）。
# 組込み/freestanding構成ではスタック保護もFORTIFYも存在しないため、
# HOSTED_CFLAGSとして分離し、CFLAGSの既定値にのみ含める。
#   -ftrivial-auto-var-init=zero ... 自動変数を常にゼロ初期化する。未初期化読み出しの
#       温床（不定値の分岐・ポインタ）を決定的に潰し、再現しない不具合を減らす。
#       GCC12+/clang8+ で利用可能。-ftrivial-auto-var-init=pattern に差し替えると
#       未初期化読み出しを 0xFE で露出させられる（tests/avinit_differential_alpha10.sh）。
ifeq ($(IS_TCC),1)
HOSTED_HARDEN ?=
else
HOSTED_HARDEN ?= -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 \
	-ftrivial-auto-var-init=zero
endif
ifeq ($(IS_TCC),1)
CFLAGS ?= $(TCC_CFLAGS)
else
CFLAGS ?= -std=c11 -O2 $(HOSTED_HARDEN) $(WARN_CFLAGS)
endif
CPPFLAGS ?= -I./include
# 依存関係生成。TinyCCは -MMD/-MP を持たないため、tcc構成では DEPFLAGS= で無効化する。
ifeq ($(IS_TCC),1)
DEPFLAGS ?=
else
DEPFLAGS ?= -MMD -MP
endif
# ホストCLI/GUIだけに適用するリンク時硬化（ELF向け）。relro/now（遅延解決をやめて
# GOT書き換えを防止）と noexecstack（スタック実行の禁止）はLinuxでは常に妥当なので
# 既定で有効にし、対象外の環境（macOS等）では自動的に無効化する。
# `make HOSTED_LDFLAGS=...` で明示的に上書きできる。
# 注意: $(if ...) の引数はカンマ区切りなので、カンマを含む値は変数へ入れてから渡す
#       （直接書くと "-Wl" だけが渡ってしまう）。
HOSTED_RELRO_LDFLAGS := -Wl,-z,relro,-z,now,-z,noexecstack
HOSTED_LDFLAGS ?= $(if $(IS_TCC),,$(if $(filter Linux,$(shell uname -s 2>/dev/null)),$(HOSTED_RELRO_LDFLAGS),))
LDFLAGS ?=
LDLIBS ?= -lm
SANITIZER_CFLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer -g
# 変換済みCコードは、内包表記などの式位置一時構築でGNU statement expressionを
# 使うため、プロジェクト本体の -Wpedantic 基準ではなく、既存の回帰テストと
# 同じ「生成コード基準」でコンパイルする。ISO C11だけを対象にする場合は
# --c11 でその構文を変換時に拒否する。
# Round-5 で生成Cにも品質基準を追加した（生成Cは利用者に渡る成果物なので、
# 本体と同じ「実バグを捕まえる」警告を当てる）。コーパス全体で警告0を確認済み。
#   -Wclobbered          : for/ジェネレータ反復が P2C_SETJMP を挟むため必須。
#   -Wshadow=local       : 関数内でローカルが別のローカルを影にする場合だけを検出。
#                          Python では関数引数がモジュール変数を隠すのは正常な
#                          スコープ規則なので、-Wshadow（global 含む）だと誤検知になる。
#   -Wcast-qual / -Wwrite-strings : 文字列リテラルや const の書き換えを検出。
#   -Wstrict-prototypes / -Wmissing-prototypes / -Wmissing-declarations /
#     -Wredundant-decls  : 生成する静的関数の宣言規律（重複宣言を実際に1件検出）。
#   -Wundef / -Wpointer-arith / -Wcast-align=strict / -Wvla :
#     移植性（未定義マクロ・ポインタ演算・アラインメント・可変長配列）。
#   -Wtautological-compare / -Winit-self / -Wstrict-aliasing=2 : 式の取り違え。
# -Wconversion は生成Cには適用しない（int64_t/size_t の相互変換を意図的に多用する
# ため、意味のない指摘が大量に出る）。
GENERATED_CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror \
	-Wshadow=local -Wclobbered -Wcast-qual -Wwrite-strings \
	-Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations \
	-Wredundant-decls -Wundef -Wpointer-arith -Wcast-align=strict -Wvla \
	-Wtautological-compare -Winit-self -Wstrict-aliasing=2
# 自作OS統合テスト(embed)用: 標準Cライブラリのプラットフォーム層を使わず、
# p2c_embed が互換フックと malloc/free を提供する構成でビルドする。
EMBED_CFLAGS ?= -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS)
# ホスト側（カーネル相当のタスク情報を提供する側）は標準Cライブラリ込みでビルドする。
HOST_STACK_CFLAGS ?= -std=c11 -O2 $(WARN_CFLAGS)
SANITIZER_ENV ?= ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# --- 追加の厳格プロファイル（開発機/CIでのバグ狩り専用。既定ビルドは不変）------
# 既定CFLAGSへ入れると freestanding/tcc/C99/ELF の移植性を壊すため、専用ターゲット
# （make test-asan-strict / test-ubsan-deep / test-msan / test-avinit-differential 等）
# からのみ使う。既定の警告基準は WARN_CFLAGS（-Wconversion 等を含む）で既に最大級
# なので、ここでは「動的解析」と「リンク時硬化」の軸を足す。
#   -fhardened             ... GCC14+ の総合硬化（FORTIFY3/PIE/CF保護/zero初期化）
#   -fstrict-flex-arrays=3 ... 末尾可変長配列の範囲外アクセスを厳格に扱う
#   -ftrivial-auto-var-init=pattern ... 自動変数を 0xFE で埋め、未初期化読み出しを
#                                      決定的に露出させる（差分ハーネスで検出）
HARDENED_CFLAGS ?= -fhardened -fstrict-flex-arrays=3
HARDENED_LDFLAGS ?= -Wl,-z,relro,-z,now,-z,noexecstack
AVINIT_CFLAGS ?= -ftrivial-auto-var-init=pattern
STRICT_SAN_CC ?= clang
# TinyCC の成果物は build/tcc と obj/tcc に分ける（GCC の成果物と混ざらないように）。
BUILD ?= $(if $(IS_TCC),build/tcc,build)
OBJ ?= $(if $(IS_TCC),obj/tcc,obj)
# テストスクリプトへ出力先を渡す（scripts は P2C_BUILD_DIR を既定の build/ より優先する）。
export P2C_BUILD_DIR := $(BUILD)

# GCC/Clang 専用の検証ゲート。TinyCC は -fsanitize / -fanalyzer / -Wstack-usage /
# -fhardened / -ftrivial-auto-var-init=pattern / ld --wrap を実際には実行せず黙って通すため、
# そのまま走らせると偽の合格になる。tcc ではここで明示的に失敗させる。
define gnu_only_gate
if [ "$(IS_TCC)" = 1 ]; then printf '%s\n' "$(1): needs GCC or Clang ($(2)); TinyCC cannot enforce it. Run: make CC=gcc $(1)" >&2; exit 2; fi
endef

# The GUI frontend has its own main(); keep it out of the default CLI link.
# src/gui は描画コアをランタイム（python_code_to_c_runtime.c）経由で取り込むため、ここには含めない
SRC := $(shell find src -type f -name '*.c' ! -path 'src/tools/gui_main.c' ! -path 'src/gui/*' | sort)
OBJFILES := $(patsubst src/%.c,$(OBJ)/%.o,$(SRC))
DEPS := $(OBJFILES:.o=.d)
# binutils（readelf/nm/file）に依存しない ELF 検査器。使用中のコンパイラでビルドする。
ELFCHECK := $(BUILD)/tools/p2c_elfcheck
ELFCHECK_CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic

.PHONY: all py2c help check-tools gui run run-gui test-gui-core test-gui-lib freestanding freestanding-clean single-header check-single-header test-single-header test-single-header-c11 test-single-header-c99 test-single-header-tcc test-single-header-freestanding full-build test test-sandbox test-sanitizers test-asan-strict test-ubsan-deep test-msan test-lsan-generated test-clang-integer test-harden-generated test-hardened-core test-avinit-differential test-warn-clang test-clang-build test-strict-profiles test-parser-sanitizers test-starred-unpack test-set test-set-comprehension-c11 test-decorator-diagnostics test-conformance test-container-fuzz test-portability test-gc-gui test-gc-lifecycle test-static-literals test-runtime-instances test-int-raw-ops test-gc-allocation-failure test-probe test-limits test-fallback test-gc-adaptive test-gc-hardening test-gc-leaks test-integer-overflow test-platform-adapter test-allocator-injection test-setjmp-hook test-heap-unification test-generator-async-runtime test-async-generator test-py313-syntax test-baremetal-runtime test-embed-baseline test-embed-runtime test-freestanding-setjmp test-embed-compile test-crlf test-hobby-os-template test-embed-generated test-baremetal-exceptions test-gc-stack-scan-scope test-gc-temp-roots test-stack-usage test-sanitizers-core test-analyzer test-analyzer-clang test-opt-hints test-stack-budget test-baremetal-build test-baremetal-generated c99 check-tcc tcc test-c99 test-tcc-freestanding test-tcc elf elfcheck elf-full tcc-elf tcc-hobbyos-elf hobby hobbyos hobbyos-elf test-hobbyos-libc install clean clean-all unicode-tables check-unicode-tables test-gui-web-security test-threads test-c2py tcc-full

all: $(BUILD)/python-code-to-c
	@mkdir -p bin
	@ln -sf ../$(BUILD)/python-code-to-c bin/python_code_to_c

# 初心者向けの短い入口。README とヘルプでは `py2c` の名前で案内しているため、
# 同じ実行ファイルを build/py2c としても置く（argv[0] をそのままヘルプに使うので、
# `py2c help` と打てば py2c の名前で使い方が表示される）。
py2c: all
	@cp $(BUILD)/python-code-to-c $(BUILD)/py2c
	@mkdir -p bin
	@ln -sf ../$(BUILD)/py2c bin/py2c
	@printf '%s\n' "py2c_ok: $(BUILD)/py2c (使い方: $(BUILD)/py2c help)"

help:
	@printf '%s\n' 'Python Code to C Beta0.1 (Py2C B0.1) build targets:'
	@printf '%s\n' '使い方: make CC=tcc <target> で TinyCC 版になる（BUILD=build/tcc に分離）。make tcc-full は TinyCC だけで全工程を実行する。'
	@printf '%s\n' 'GCC/Clang 専用の検証は、TinyCC では実行せず明示的に失敗する（偽の合格を防ぐため）。'
	@printf '%s\n' '[基本ビルド]'
	@printf '%s\n' '  make all                                 ホスト CLI（BUILD/python-code-to-c）をビルドし bin/ へリンクする'
	@printf '%s\n' '  make py2c                                all に加えて build/py2c と bin/py2c を用意する（初心者向けの入口）'
	@printf '%s\n' '  make gui                                 ローカル GUI フロントエンド（BUILD/python-code-to-c-gui）をビルドする'
	@printf '%s\n' '  make run INPUT=path/to/file.py           変換・C コンパイル・実行を一括で行う（既定は examples/fib.py）'
	@printf '%s\n' '  make run-gui                             GUI をビルドして起動する（対話的。timeout 付きで実行可能）'
	@printf '%s\n' '  make full-build                          CLI・GUI・freestanding・単一ヘッダーをクリーンビルドする'
	@printf '%s\n' '  make check-tools                         CC/AR/PYTHON の存在と、CC の種別・版を表示する'
	@printf '%s\n' '  make elfcheck                            binutils 非依存の ELF 検査器（tools/p2c_elfcheck.c）をビルドする'
	@printf '%s\n' '  make install                             CLI を DESTDIR 配下の /usr/local/bin へ入れる'
	@printf '%s\n' '  make clean                               現在の BUILD と OBJ を消す（TinyCC では build/tcc と obj/tcc）'
	@printf '%s\n' '  make clean-all                           build/ と obj/ を全て消す'
	@printf '%s\n' '  make help                                このヘルプを表示する'
	@printf '%s\n' '[TinyCC だけで作る（GCC・clang・binutils 不要）]'
	@printf '%s\n' '  make tcc-full                            TinyCC だけで全工程を実行する（ビルド・全テスト・ELF・HobbyOS ELF）'
	@printf '%s\n' '  make tcc                                 TinyCC（TCC=tcc）で CLI と GUI をビルドする（build/tcc）'
	@printf '%s\n' '  make check-tcc                           TinyCC が使えるか確認する（TCC=/path/to/tcc で場所を指定）'
	@printf '%s\n' '  make test-tcc                            TinyCC で変換器と生成 C の両方を通す差分テスト（単一ヘッダー・freestanding を含む）'
	@printf '%s\n' '  make test-tcc-freestanding               NO_STDLIB のコア・テンプレート・setjmp を TinyCC でコンパイルする'
	@printf '%s\n' '  make test-single-header-tcc              単一ヘッダーを TinyCC で hosted 実行し、freestanding をコンパイルする'
	@printf '%s\n' '  make c99                                 CLI と GUI を厳格 C99（-std=c99 -pedantic）でビルドする'
	@printf '%s\n' '  make test-c99                            C99 でビルドした変換器で差分コーパス全体を実行する'
	@printf '%s\n' '[単一ヘッダー・freestanding]'
	@printf '%s\n' '  make freestanding CC=<cross-cc> AR=<cross-ar>  C11 freestanding コア（BUILD/freestanding の .a）をビルドする'
	@printf '%s\n' '  make freestanding-clean                  freestanding の成果物を消す'
	@printf '%s\n' '  make single-header                       include/python_code_to_c_single.h を現行ソースから再生成する'
	@printf '%s\n' '  make check-single-header                 コミット済みの単一ヘッダーが現行ソースの再生成結果と一致するか検査する'
	@printf '%s\n' '  make unicode-tables                            Regenerate the Unicode tables from CPython str predicates and case maps.'
	@printf '%s\n' '  make check-unicode-tables                      Check that the generated Unicode tables are up to date.'
	@printf '%s\n' '  make test-threads                              Run the compiler and per-thread runtime contexts under pthreads.'
	@printf '%s\n' '  make test-gui-web-security                     Check the Web GUI token/Host/Origin/limits and the HTTP server.'
	@printf '%s\n' '  make test-single-header                  単一ヘッダーを既定コンパイラでビルド・実行する'
	@printf '%s\n' '  make test-single-header-c11              単一ヘッダーを -pedantic-errors で検査する'
	@printf '%s\n' '  make test-single-header-c99              単一ヘッダーを GCC の -std=c99 -pedantic-errors で検査する'
	@printf '%s\n' '  make test-single-header-freestanding     NO_STDLIB 構成で hosted libc への参照が無いことを検査する'
	@printf '%s\n' '[変換器・言語機能の回帰]'
	@printf '%s\n' '  make test                                全回帰を既定の CC で実行する（TinyCC では GNU 専用ゲートを明示的に省いて表示）'
	@printf '%s\n' '  make test-conformance                    CPython 差分コーパス（868 アサーション）'
	@printf '%s\n' '  make test-container-fuzz                 dict/set の CPython 差分ファジング（seed 固定）'
	@printf '%s\n' '  make test-async-generator                ジェネレータ・async の CPython 差分スモーク'
	@printf '%s\n' '  make test-py313-syntax                   Python 3.13 の type 文・型パラメータを C11 変換で検証する'
	@printf '%s\n' '  make test-starred-unpack                 starred unpack 代入の回帰'
	@printf '%s\n' '  make test-set                            set の回帰（tests/set_regression.sh）'
	@printf '%s\n' '  make test-set-comprehension-c11          set 内包表記が --c11 で明示的に拒否されることを検査する'
	@printf '%s\n' '  make test-decorator-diagnostics          デコレータの診断メッセージを検査する'
	@printf '%s\n' '  make test-probe                          意味論プローブ（tests/semantic_probe_alpha10.sh）を実行する'
	@printf '%s\n' '  make test-limits                         上限（limit）診断を検査する'
	@printf '%s\n' '  make test-fallback                       --fallback の挙動（診断・未到達・到達して NotImplementedError）を検査する'
	@printf '%s\n' '  make test-crlf                           LF/CRLF/CR のソースから同一の C が生成されることを検査する'
	@printf '%s\n' '  make test-portability                    GUI と freestanding の成果物が揃うことを検査する'
	@printf '%s\n' '  make test-sandbox                        サンドボックスの予算超過が SandboxError になり、ハングしないことを検査する'
	@printf '%s\n' '  make test-hobbyos-libc                   HobbyOS 参照 libm/libc をホストと比較する'
	@printf '%s\n' '[ランタイム・GC]'
	@printf '%s\n' '  make test-gc-gui                         GUI 関連の GC C 回帰'
	@printf '%s\n' '  make test-gc-lifecycle                   GC のライフサイクル（再初期化を含む）'
	@printf '%s\n' '  make test-gc-stack-scan-scope            保守的スタック走査が使用範囲内に収まることを検査する'
	@printf '%s\n' '  make test-gc-temp-roots                  式評価中の一時値が GC ルートになることを検査する'
	@printf '%s\n' '  make test-gc-allocation-failure          確保失敗経路を --wrap で検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-gc-adaptive                    適応 GC のしきい値調整を検査する'
	@printf '%s\n' '  make test-gc-hardening                   GC のルート動的拡張と非再帰マークを検査する'
	@printf '%s\n' '  make test-gc-leaks                       LeakSanitizer で GC の解放漏れを検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-static-literals                不変の静的リテラル（P2C_STATIC_STR）を検査する'
	@printf '%s\n' '  make test-runtime-instances              ランタイムインスタンスの分離を検査する'
	@printf '%s\n' '  make test-int-raw-ops                    生の int64 演算（アンボクシング）と P2C_Object 版の一致を検査する'
	@printf '%s\n' '  make test-integer-overflow               整数オーバーフロー時の OverflowError を検査する'
	@printf '%s\n' '  make test-platform-adapter               P2C_Platform アダプタを検査する'
	@printf '%s\n' '  make test-allocator-injection            アロケータ注入（p2c_platform_set_allocator）を検査する'
	@printf '%s\n' '  make test-setjmp-hook                    P2C_SETJMP/P2C_LONGJMP のフックを検査する'
	@printf '%s\n' '  make test-heap-unification               スタブ malloc がプラットフォームヒープへ委譲されることを検査する'
	@printf '%s\n' '  make test-generator-async-runtime        ジェネレータ・async のランタイム C 回帰'
	@printf '%s\n' '  make test-stack-usage                    フレームが STACK_USAGE_LIMIT 以下であることを検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-stack-budget                   組込みコアを 4096 バイトのスタック予算で検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-opt-hints                      性能ヒント（const/pure など）を助言として表示する [GCC 専用]'
	@printf '%s\n' '[組込み・HobbyOS・ベアメタル]'
	@printf '%s\n' '  make test-embed-baseline                 p2c_embed_start() に必要なヒープ量を実測する'
	@printf '%s\n' '  make test-embed-runtime                  組込みヒープ・ライフサイクル・OOM・panic の回帰'
	@printf '%s\n' '  make test-freestanding-setjmp            カーネル提供 setjmp/longjmp による例外の回帰'
	@printf '%s\n' '  make test-embed-compile                  NO_STDLIB 構成で変換器コアを実行する（オンデバイス変換）'
	@printf '%s\n' '  make test-embed-generated                --embed-entry の生成モジュールをカーネル相当ドライバで実行し CPython と比較する（x86-64）'
	@printf '%s\n' '  make test-hobby-os-template              HobbyOS テンプレートを警告即エラーでコンパイルし、ライブラリ化する'
	@printf '%s\n' '  make test-baremetal-runtime              ベアメタル例（静的ヒープ・GC・コルーチン）を検査する'
	@printf '%s\n' '  make test-baremetal-exceptions           ベアメタル例の例外機構・スタック根・プラットフォームアロケータを検査する'
	@printf '%s\n' '  make test-baremetal-build                ベアメタル用テンプレートをライブラリ化する'
	@printf '%s\n' '  make test-baremetal-generated            ベアメタル例が freestanding でオブジェクト化できることを検査する'
	@printf '%s\n' '[ELF]'
	@printf '%s\n' '  make elf                                 ELF を作る。CC=tcc なら TinyCC 版、goal に hobby/hobbyos があれば HobbyOS 版'
	@printf '%s\n' '  make elf-full                            GCC/Clang: 静的リンクの CLI ELF（BUILD/elf）を作り検査する'
	@printf '%s\n' '  make tcc-elf                             TinyCC: CLI の ELF（動的リンク。TinyCC は glibc を静的リンクできない）'
	@printf '%s\n' '  make hobbyos elf                         HobbyOS 向け ELF（libc なし・静的・W^X・未定義シンボル無し）'
	@printf '%s\n' '  make hobbyos-elf                         同上（明示的なターゲット名。GCC/Clang のリンカスクリプトを使う）'
	@printf '%s\n' '  make tcc-hobbyos-elf                     TinyCC: HobbyOS 向け ELF（-static -nostdlib、ENTRY は _start の薄いシム）'
	@printf '%s\n' '  make hobby                               hobbyos の別名（`make tcc elf hobby c99` などで使う）'
	@printf '%s\n' '  make hobbyos                             `make hobbyos elf` の使い方を表示する'
	@printf '%s\n' '  make tcc elf c99                         TinyCC（C99）で完全機能の CLI ELF を作り、検査する'
	@printf '%s\n' '  make tcc elf hobby c99                   TinyCC（C99）で HobbyOS 向け ELF を作り、検査する'
	@printf '%s\n' '[GCC/Clang 専用の検証（TinyCC では明示的に失敗する）]'
	@printf '%s\n' '  make test-sanitizers                     ホスト実行系を ASan/UBSan で検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-parser-sanitizers              構文解析器を ASan で検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-sanitizers-core                変換器本体を ASan/UBSan でビルドして変換パイプラインを通す [GCC/Clang 専用]'
	@printf '%s\n' '  make test-asan-strict                    厳格 ASan（use-after-return など）で差分コーパスを通す [GCC/Clang 専用]'
	@printf '%s\n' '  make test-ubsan-deep                     最大限の UBSan で差分コーパスを通す [GCC/Clang 専用]'
	@printf '%s\n' '  make test-msan                           clang MemorySanitizer で未初期化読み出しを検査する [clang 専用]'
	@printf '%s\n' '  make test-lsan-generated                 生成コードを LeakSanitizer で検査する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-clang-integer                  clang の整数変換サニタイザで検査する [clang 専用]'
	@printf '%s\n' '  make test-harden-generated               生成プログラムを _FORTIFY_SOURCE=3 とスタック保護でビルドする [GCC/Clang 専用]'
	@printf '%s\n' '  make test-hardened-core                  変換器を -fhardened でビルドし差分コーパスを通す [GCC 14+ 専用]'
	@printf '%s\n' '  make test-avinit-differential            未初期化スタック読み出しを pattern 初期化の差分で検出する [GCC/Clang 専用]'
	@printf '%s\n' '  make test-analyzer                       GCC -fanalyzer で全ソースを静的解析する [GCC 専用]'
	@printf '%s\n' '  make test-analyzer-clang                 clang の静的解析器で全ソースを検査する [clang 専用]'
	@printf '%s\n' '  make test-warn-clang                     clang の追加警告で全ソースを検査する [clang 専用]'
	@printf '%s\n' '  make test-clang-build                    clang で変換器をビルドし差分コーパスを通す [clang 専用]'
	@printf '%s\n' '  make test-strict-profiles                上の厳格プロファイルを全て実行する（時間がかかる）[GCC/clang 専用]'

check-tools:
	@command -v "$(firstword $(CC))" >/dev/null || { printf '%s\n' "compiler not found: $(CC)" >&2; exit 1; }
	@command -v "$(firstword $(AR))" >/dev/null || { printf '%s\n' "archiver not found: $(AR)" >&2; exit 1; }
	@command -v "$(firstword $(PYTHON))" >/dev/null || { printf '%s\n' "Python launcher not found: $(PYTHON)" >&2; exit 1; }
	@printf '%s\n' "toolchain_ok: CC=$(CC) kind=$(CC_KIND) AR=$(AR) PYTHON=$(PYTHON)"
	@printf '%s\n' "toolchain_version: $(CC_VERSION_LINE)"

gui: $(BUILD)/python-code-to-c-gui

run: all
	./$(BUILD)/python-code-to-c run "$(INPUT)"

run-gui: gui
	./$(BUILD)/python-code-to-c-gui

GUI_OBJFILES := $(filter-out $(OBJ)/tools/main.o,$(OBJFILES))

$(BUILD)/python-code-to-c-gui: $(GUI_OBJFILES) $(OBJ)/tools/gui_main.o
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $(HOSTED_LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/python-code-to-c: $(OBJFILES)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $(HOSTED_LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Build the compiler core without hosted OS facilities. A target OS supplies
# allocation, I/O, diagnostics and process hooks through platform callbacks.
freestanding:
	$(MAKE) -C "$(CURDIR)" -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. BUILD="$(BUILD)/freestanding" CC="$(CC)" AR="$(AR)" WARN_CFLAGS="$(WARN_CFLAGS)" FREESTANDING_ARCH_CFLAGS="$(FREESTANDING_ARCH_CFLAGS)" EXTRA_CFLAGS="$(FREESTANDING_EXTRA_CFLAGS)"

freestanding-clean:
	$(MAKE) -C "$(CURDIR)" -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. BUILD=$(BUILD)/freestanding clean

single-header:
	sh tools/generate_single_header.sh

# コミット済みの単一ヘッダーが、現行ソースから再生成したものと一致することを確認する。
unicode-tables:
	$(PYTHON) tools/gen_unicode_tables.py

check-unicode-tables:
	@$(PYTHON) tools/gen_unicode_tables.py --check || { printf '%s\n' 'Unicode tables are stale: run make unicode-tables' >&2; exit 1; }

# Web GUI のセキュリティ（トークン・Host/Origin・資源制限）と HTTP サーバーの並行性を、
# 実際の GUI バイナリに対して検査する（tests/gui_web_security_alpha10.py）。
test-gui-web-security: gui
	$(PYTHON) tests/gui_web_security_alpha10.py $(BUILD)/python-code-to-c-gui

check-single-header:
	@mkdir -p $(BUILD)/single_check
	@sh tools/generate_single_header.sh $(BUILD)/single_check/python_code_to_c_single.h >/dev/null
	@cmp -s include/python_code_to_c_single.h $(BUILD)/single_check/python_code_to_c_single.h || { printf '%s\n' 'include/python_code_to_c_single.h is stale: run make single-header' >&2; exit 1; }
	@printf '%s\n' 'single_header_fresh_ok'

test-single-header: single-header
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_single_header.c $(LDLIBS) -o $(BUILD)/tests/test_single_header
	$(BUILD)/tests/test_single_header

test-single-header-c11: single-header
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -pedantic-errors tests/test_single_header.c $(LDLIBS) -o $(BUILD)/tests/test_single_header_c11
	$(BUILD)/tests/test_single_header_c11

# 単一ヘッダーがC99（GCC -pedantic-errors）でも完全にビルド・実行できることを確認する。
test-single-header-c99: single-header
	@mkdir -p build/tests
	$(CC) -I./include -std=c99 -pedantic-errors -Wall -Wextra -Werror -O2 \
		tests/test_single_header.c $(LDLIBS) -o $(BUILD)/tests/test_single_header_c99
	$(BUILD)/tests/test_single_header_c99
	@printf '%s\n' 'single_header_c99_ok'

# 単一ヘッダーがTinyCCでも完全にビルドできることを確認する（hosted + freestanding）。
# tccは -ffreestanding を持たないため、NO_STDLIB定義だけで freestanding 構成を作る。
test-single-header-tcc: single-header check-tcc
	$(MAKE) CC="$(TCC)" BUILD="$(TCC_BUILD)" OBJ="$(TCC_OBJ)" DEPFLAGS= elfcheck
	@mkdir -p build/tests
	$(TCC) -I./include -std=c99 -O2 -Wall -Werror \
		tests/test_single_header.c $(LDLIBS) -o $(BUILD)/tests/test_single_header_tcc
	$(BUILD)/tests/test_single_header_tcc
	$(TCC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -std=c99 -O2 -Wall -Werror \
		-c tests/test_single_header_freestanding.c -o $(BUILD)/tests/test_single_header_freestanding_tcc.o
	@if $(TCC_BUILD)/tools/p2c_elfcheck --undef $(BUILD)/tests/test_single_header_freestanding_tcc.o | grep -E '^(malloc|realloc|free|fwrite|fputs|clock)$$' >/dev/null; then \
		echo 'tcc freestanding single header has hosted libc references' >&2; exit 1; \
	fi
	@printf '%s\n' 'single_header_tcc_ok'

test-single-header-freestanding: single-header $(ELFCHECK)
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -c tests/test_single_header_freestanding.c -o $(BUILD)/tests/test_single_header_freestanding.o
	@if $(ELFCHECK) --undef $(BUILD)/tests/test_single_header_freestanding.o | grep -E '^(malloc|realloc|free|fwrite|fputs|clock)$$' >/dev/null; then echo 'freestanding single header has hosted libc references'; exit 1; fi

full-build: check-tools
	$(MAKE) clean
	$(MAKE) CC="$(CC)" AR="$(AR)" all gui
	$(MAKE) CC="$(CC)" AR="$(AR)" freestanding
	$(MAKE) CC="$(CC)" AR="$(AR)" single-header

test: all
	CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/smoke.sh
	$(MAKE) check-single-header
	$(MAKE) check-unicode-tables
	$(MAKE) test-gc-gui
	$(MAKE) test-gc-lifecycle
	$(MAKE) test-gc-stack-scan-scope
	$(MAKE) test-gc-temp-roots
	$(MAKE) test-static-literals
	$(MAKE) test-runtime-instances
	$(MAKE) test-int-raw-ops
	$(MAKE) test-integer-overflow
	$(MAKE) test-platform-adapter
	$(MAKE) test-allocator-injection
	$(MAKE) test-threads
	$(MAKE) test-gui-web-security
	$(MAKE) test-gui-core
	$(MAKE) test-gui-lib
	$(MAKE) test-c2py
	$(MAKE) test-setjmp-hook
	$(MAKE) test-heap-unification
	$(MAKE) test-generator-async-runtime
	$(MAKE) test-async-generator
	$(MAKE) test-py313-syntax
	$(MAKE) test-baremetal-runtime
	$(MAKE) test-baremetal-exceptions
	$(MAKE) test-baremetal-build
	$(MAKE) test-baremetal-generated
	$(MAKE) test-embed-runtime
	$(MAKE) test-freestanding-setjmp
	$(MAKE) test-embed-compile
	$(MAKE) test-embed-generated
	$(MAKE) test-crlf
	$(MAKE) test-hobby-os-template
	$(MAKE) test-starred-unpack
	$(MAKE) test-set
	$(MAKE) test-set-comprehension-c11
	$(MAKE) test-decorator-diagnostics
	$(MAKE) test-single-header
	$(MAKE) test-single-header-c11
	$(MAKE) test-single-header-c99
	$(MAKE) test-single-header-freestanding
	$(MAKE) test-sandbox
	$(MAKE) test-gc-adaptive
	$(MAKE) test-gc-hardening
	$(MAKE) test-fallback
	$(MAKE) test-hobbyos-libc
	$(MAKE) test-c99
	$(MAKE) test-conformance
	$(MAKE) test-container-fuzz
	CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/audit_regression.sh
	$(MAKE) test-portability
ifneq ($(IS_TCC),1)
	$(MAKE) test-stack-usage
	$(MAKE) test-stack-budget
	$(MAKE) test-gc-allocation-failure
else
	@printf '%s\n' 'test: TinyCC run. Skipped GNU/Clang-only gates (run them with CC=gcc): test-stack-usage test-stack-budget test-gc-allocation-failure'
endif

# サンドボックス（実行予算: ステップ/確保）の回帰。予算超過が SandboxError で
# 止まること（ハング・クラッシュにしない）と、生成whileループの計装を確認する。
test-sandbox: all
	CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/sandbox_regression_alpha10.sh

test-sanitizers:
	@$(call gnu_only_gate,test-sanitizers,-fsanitize)
	@# 既定の build/ を消さないよう、サニタイザ専用ディレクトリだけを掃除する
	@# （以前は `make clean` を呼んでおり、通常ビルドの成果物まで消えていた）。
	@rm -rf build/sanitize obj/sanitize
	@mkdir -p build/tests
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-parser-sanitizers
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-gc-lifecycle
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-gc-allocation-failure
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-static-literals
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-runtime-instances
	$(MAKE) BUILD=build/sanitize OBJ=obj/sanitize CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" test-int-raw-ops
	$(SANITIZER_ENV) P2C_COMPILER=./build/sanitize/python-code-to-c P2C_TEST_CFLAGS="-Wall -Wextra -Werror -std=gnu11 $(SANITIZER_CFLAGS)" P2C_TEST_LDFLAGS="$(SANITIZER_CFLAGS)" CC="$(CC)" sh tests/conformance_regression.sh

# --- 厳格プロファイル（バグ狩り） ----------------------------------------------
# 生成プログラム+ランタイムを各サニタイザ構成でビルドし、CPython差分コーパス
# （749アサーション）と意味論プローブ（61件）を実行する。
# 各プロファイルの内容は tests/strict_dynamic_alpha10.sh を参照。
test-asan-strict: all
	@$(call gnu_only_gate,test-asan-strict,-fsanitize)
	sh tests/strict_dynamic_alpha10.sh asan-strict

test-ubsan-deep: all
	@$(call gnu_only_gate,test-ubsan-deep,-fsanitize)
	sh tests/strict_dynamic_alpha10.sh ubsan-deep

test-msan: all
	@$(call gnu_only_gate,test-msan,clang MemorySanitizer)
	CC=$(STRICT_SAN_CC) sh tests/strict_dynamic_alpha10.sh msan

test-lsan-generated: all
	@$(call gnu_only_gate,test-lsan-generated,-fsanitize)
	sh tests/strict_dynamic_alpha10.sh lsan

test-clang-integer: all
	@$(call gnu_only_gate,test-clang-integer,clang sanitizers)
	CC=$(STRICT_SAN_CC) sh tests/strict_dynamic_alpha10.sh clang-int

test-harden-generated: all
	@$(call gnu_only_gate,test-harden-generated,-D_FORTIFY_SOURCE=3 and sanitizers)
	sh tests/strict_dynamic_alpha10.sh harden

# 変換器本体を -fhardened でビルドし、その変換器でコーパスを走らせる。
test-hardened-core:
	@$(call gnu_only_gate,test-hardened-core,-fhardened)
	$(MAKE) BUILD=build/hardened OBJ=obj/hardened \
		CFLAGS="$(CFLAGS) $(HARDENED_CFLAGS)" LDFLAGS="$(LDFLAGS) $(HARDENED_LDFLAGS)" all
	P2C_COMPILER=./build/hardened/python-code-to-c sh tests/conformance_regression.sh
	P2C_COMPILER=./build/hardened/python-code-to-c sh tests/semantic_probe_alpha10.sh

# 未初期化スタック読み出しの差分検出（変換器の出力 + 生成プログラムの実行結果）。
test-avinit-differential: all
	@$(call gnu_only_gate,test-avinit-differential,-ftrivial-auto-var-init=pattern)
	$(MAKE) BUILD=build/avinit OBJ=obj/avinit CFLAGS="$(CFLAGS) $(AVINIT_CFLAGS)" all
	AVINIT_RUN=1 sh tests/avinit_differential_alpha10.sh ./$(BUILD)/python-code-to-c ./build/avinit/python-code-to-c

# clang を「第二の警告源」として全ソースに当てる。GCC の WARN_CFLAGS では
# 拾えない軸（64→32切り詰め、条件付き未初期化、enum 変換、到達不能コード）を補完する。
CLANG_EXTRA_WARN ?= -Wshorten-64-to-32 -Wconditional-uninitialized -Wenum-conversion \
	-Wimplicit-int-conversion -Wshadow-all -Wunreachable-code -Wassign-enum -Wcomma \
	-Wabsolute-value -Wloop-analysis -Wsizeof-array-decay -Wformat-non-iso
test-warn-clang: all
	@$(call gnu_only_gate,test-warn-clang,clang warnings)
	@command -v $(firstword $(CLANG)) > /dev/null || { printf '%s\n' "$(CLANG) not found (optional cross-check)"; exit 1; }
	@mkdir -p $(BUILD)/tests
	@for src in $(SRC); do \
		out=$(BUILD)/tests/clangwarn_$$(basename $$src .c).o; \
		$(CLANG) -I./include -std=c11 -O1 -Wall -Wextra -Werror \
			-Wcast-qual -Wvla -Wpointer-arith -Wcast-align \
			-Wstrict-prototypes -Wmissing-prototypes \
			$(CLANG_EXTRA_WARN) -c $$src -o $$out || exit 1; \
	done
	@printf '%s\n' 'clang_warn_ok: second-opinion clang warnings are clean'

# clang で変換器本体をビルドし、その変換器でコーパスを走らせる（第二コンパイラ）。
test-clang-build:
	@$(call gnu_only_gate,test-clang-build,clang build)
	sh tests/clang_build_alpha10.sh

# 動的解析系の厳格プロファイルをまとめて実行する（時間がかかる）。
test-strict-profiles: all
	@$(call gnu_only_gate,test-strict-profiles,sanitizer and analyzer profiles)
	$(MAKE) test-warn-clang
	$(MAKE) test-clang-build
	$(MAKE) test-asan-strict
	$(MAKE) test-ubsan-deep
	$(MAKE) test-clang-integer
	$(MAKE) test-harden-generated
	$(MAKE) test-hardened-core
	$(MAKE) test-avinit-differential

test-parser-sanitizers: all
	@$(call gnu_only_gate,test-parser-sanitizers,-fsanitize)
	@mkdir -p $(BUILD)/tests
	$(SANITIZER_ENV) ./$(BUILD)/python-code-to-c tests/parser_starred_unpack_asan_alpha10.py -o $(BUILD)/tests/parser_starred_unpack_asan_alpha10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD)/tests/parser_starred_unpack_asan_alpha10.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/parser_starred_unpack_asan_alpha10
	$(SANITIZER_ENV) $(BUILD)/tests/parser_starred_unpack_asan_alpha10 > $(BUILD)/tests/parser_starred_unpack_asan_alpha10.out
	printf '10 [20, 30] 40\n1 (2, 3, 4) 5\n' > $(BUILD)/tests/parser_starred_unpack_asan_alpha10.expected
	diff -u $(BUILD)/tests/parser_starred_unpack_asan_alpha10.expected $(BUILD)/tests/parser_starred_unpack_asan_alpha10.out

test-starred-unpack: all
	@mkdir -p build/tests
	./$(BUILD)/python-code-to-c tests/starred_unpack_alpha10.py -o $(BUILD)/tests/starred_unpack_alpha10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD)/tests/starred_unpack_alpha10.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/starred_unpack_alpha10
	$(BUILD)/tests/starred_unpack_alpha10 > $(BUILD)/tests/starred_unpack_alpha10.out
	printf '1 [2, 3, 4] 5\n[10, 20] 30\n7 [8, 9]\n' > $(BUILD)/tests/starred_unpack_alpha10.expected
	diff -u $(BUILD)/tests/starred_unpack_alpha10.expected $(BUILD)/tests/starred_unpack_alpha10.out

test-set: all
	CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/set_regression.sh

test-set-comprehension-c11: all
	@mkdir -p build/tests
	@if ./$(BUILD)/python-code-to-c --c11 tests/set_comprehension_alpha10.py -o $(BUILD)/tests/set_comprehension_alpha10.c11.c >$(BUILD)/tests/set_comprehension_alpha10.c11.out 2>$(BUILD)/tests/set_comprehension_alpha10.c11.err; then printf '%s\n' 'set comprehension unexpectedly passed --c11' >&2; exit 1; fi
	@grep -F 'comprehensions are unavailable in strict ISO C11 mode' $(BUILD)/tests/set_comprehension_alpha10.c11.err >/dev/null
	@printf '%s\n' 'set_comprehension_c11_diagnostic_ok'

test-decorator-diagnostics: all
	P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/decorator_diagnostics_alpha10.sh

test-conformance: all
	P2C_TEST_CFLAGS="$(GENERATED_CFLAGS)" CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/conformance_regression.sh

test-container-fuzz: all
	CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" P2C_FUZZ_CASES="$(P2C_FUZZ_CASES)" P2C_FUZZ_SEEDS="$(P2C_FUZZ_SEEDS)" sh tests/container_fuzz_regression.sh

test-portability:
	$(MAKE) gui
	$(MAKE) freestanding
	test -s $(BUILD)/python-code-to-c-gui
	test -s $(BUILD)/freestanding/libpython-code-to-c-core.a
	@printf '%s\n' 'C80: GUI and freestanding portability artifacts passed'

test-gc-gui:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_gui.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/test_gc_gui
	$(BUILD)/tests/test_gc_gui

test-gc-lifecycle:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_runtime_reinit.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_gc_runtime_reinit
	$(SANITIZER_ENV) $(BUILD)/tests/test_gc_runtime_reinit

# 不変の静的リテラル（P2C_STATIC_STR）の性質を確認する。
# 生成コードはリテラルを「使うたびに確保」せず共有実体を参照するため、
# 共有・不変・GC非追跡・再初期化をまたぐ有効性をここで固定する。
test-static-literals:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_static_literals.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_static_literals
	$(SANITIZER_ENV) $(BUILD)/tests/test_static_literals

# ランタイムインスタンス（コンテキスト）の分離を確認する。
# 「カーネルのタスクごとに 1 インスタンス」という使い方の前提
# （GC・レジストリ・フリーリスト・式スタックが混ざらないこと）を固定する。
test-runtime-instances:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_runtime_instances.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_runtime_instances
	$(SANITIZER_ENV) $(BUILD)/tests/test_runtime_instances

# 生の int64 演算（AOT アンボクシングで型付きローカルから使う）が、
# P2C_Object* 版と同じ結果・同じ例外になることを固定する。
test-int-raw-ops:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_int_raw_ops.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_int_raw_ops
	$(SANITIZER_ENV) $(BUILD)/tests/test_int_raw_ops

test-gc-allocation-failure:
	@$(call gnu_only_gate,test-gc-allocation-failure,ld --wrap)
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_allocation_failure.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc $(LDLIBS) -o $(BUILD)/tests/test_gc_allocation_failure
	$(SANITIZER_ENV) $(BUILD)/tests/test_gc_allocation_failure

# 意味論プローブ（tests/semantic_probe_alpha10.sh）の回帰。
test-probe: all
	@CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/semantic_probe_alpha10.sh

# 上限（limit）診断の回帰。
test-limits: all
	@CC="$(CC)" P2C_COMPILER="$(BUILD)/python-code-to-c" sh tests/limit_diagnostics_alpha10.sh

test-fallback: all
	P2C_COMPILER=$(BUILD)/python-code-to-c CC="$(CC)" sh tests/fallback_regression_alpha10.sh

# 適応GC（しきい値の自動調整）の回帰。固定しきい値と比較して収集回数が減り、
# 追跡オブジェクト数のピークが上限内に収まることを確認する。
test-gc-adaptive:
	@mkdir -p $(BUILD)/tests
	@# このテストは「スタック上の生きた集合（20000ポインタ＝約160KiB）を保守的
	@# スキャンが保持できるか」を検証するため、意図的に巨大なフレームを持つ。
	@# 検証内容そのものがスタック配置に依存するので、この1ターゲットに限り
	@# -Wno-stack-usage を付ける（製品コードと他のテストは予算を守る）。
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_PYGAME -std=c11 -O2 $(WARN_CFLAGS) -Wno-stack-usage \
		tests/test_gc_adaptive.c src/runtime/python_code_to_c_runtime.c \
		src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c \
		src/platform/python_code_to_c_platform_hosted.c $(LDLIBS) -o $(BUILD)/tests/test_gc_adaptive
	$(BUILD)/tests/test_gc_adaptive

# GCハードニング: ルート表の動的拡張（>1024 globals）と、深い入れ子の
# 反復マーク（旧実装は再帰で深さ分だけCスタックを消費した）を回帰検証する。
test-gc-hardening:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_hardening.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/test_gc_hardening
	$(BUILD)/tests/test_gc_hardening
ifneq ($(IS_TCC),1)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_CFLAGS) tests/test_gc_hardening.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(SANITIZER_CFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_gc_hardening_lsan
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_hardening_lsan
endif

test-gc-leaks:
	@$(call gnu_only_gate,test-gc-leaks,LeakSanitizer)
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_CFLAGS) tests/test_gc_runtime_reinit.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(SANITIZER_CFLAGS) $(LDLIBS) -o $(BUILD)/tests/test_gc_runtime_reinit_lsan
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_runtime_reinit_lsan

test-integer-overflow:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_integer_overflow_runtime.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/test_integer_overflow_runtime
	$(BUILD)/tests/test_integer_overflow_runtime

test-platform-adapter:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_platform_adapter.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/test_platform_adapter
	$(BUILD)/tests/test_platform_adapter

# 変換器コア / ランタイムへのアロケータ注入（自作OSの kmalloc/arena を唯一の
# ヒープにする）と、静的フォールバックへ落ちる条件の回帰。
test-allocator-injection: all
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_allocator_injection.c \
		$(filter-out $(OBJ)/tools/main.o,$(OBJFILES)) $(LDLIBS) -o $(BUILD)/tests/test_allocator_injection
	$(BUILD)/tests/test_allocator_injection

# 並行実行の回帰（pthread）: 変換器の並行呼び出しと、スレッドごとのランタイムコンテキスト。
test-c2py: all
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude tests/test_c2py_alpha10.c src/tools/c2py.c -o $(BUILD)/tests/test_c2py_alpha10
	$(BUILD)/tests/test_c2py_alpha10

test-threads: all
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread tests/test_threads_alpha10.c $(filter-out $(OBJ)/tools/main.o,$(OBJFILES)) $(LDLIBS) -o $(BUILD)/tests/test_threads
	$(BUILD)/tests/test_threads

# setjmp/longjmp のOS差し替えフック (P2C_SETJMP/P2C_LONGJMP) の回帰。
# テスト本体とランタイム本体の両方を同じオーバーライドでコンパイルし、
# ランタイム/生成コードが本当にフックを通ることを検証する。
test-setjmp-hook: all
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -include tests/setjmp_hook_override.h \
		tests/test_setjmp_hook.c src/runtime/python_code_to_c_runtime.c \
		$(filter-out $(OBJ)/runtime/python_code_to_c_runtime.o $(OBJ)/tools/main.o,$(OBJFILES)) \
		$(LDLIBS) -o $(BUILD)/tests/test_setjmp_hook
	$(BUILD)/tests/test_setjmp_hook

# 共有ヒープの一本化（NO_STDLIB 構成）の回帰。ランタイム同梱 libc スタブの
# malloc 系がカーネルアロケータ（共有ヒープ）へ委譲されること、raw malloc と
# p2c_heap_alloc() のブロックが相互に解放できること、プラットフォーム未設定時は
# 線形ヒープが唯一のヒープになること、ヒープ未設定では確保が NULL になることを検証する。
test-gui-core:
	@mkdir -p build/tests build
	$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) \
		tests/gui_core_test.c src/gui/python_code_to_c_gui.c -o build/tests/gui_core_test
	./build/tests/gui_core_test
	$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) \
		tests/calc_core_test.c src/gui/python_code_to_c_calc.c src/gui/python_code_to_c_gui.c -o build/tests/calc_core_test
	./build/tests/calc_core_test
	$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) \
		tests/calc_app_test.c src/gui/python_code_to_c_calc_app.c src/gui/python_code_to_c_host.c \
		src/gui/python_code_to_c_hobby_fb_host.c \
		src/gui/python_code_to_c_calc.c src/gui/python_code_to_c_gui.c -o build/tests/calc_app_test
	./build/tests/calc_app_test
	$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) \
		tests/calc_hobby_fb_test.c src/gui/python_code_to_c_calc_app.c src/gui/python_code_to_c_host.c \
		src/gui/python_code_to_c_calc.c src/gui/python_code_to_c_gui.c -o build/tests/calc_hobby_fb_test
	./build/tests/calc_hobby_fb_test
	@printf '%s\n' 'test-gui-core: ok'

test-gui-lib: all
	@mkdir -p build
	./build/python-code-to-c run tests/gui_lib_calc.py > build/gui_lib_calc.out
	printf '%s\n' '15' | cmp - build/gui_lib_calc.out
	test "$$(head -c 2 build/calc_gui.ppm)" = "P6"
	@printf '%s\n' 'test-gui-lib: ok'

test-heap-unification:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) \
		tests/test_heap_unification.c src/core/python_code_to_c.c src/lexer/python_code_to_c_lexer.c \
		src/parser/python_code_to_c_ast.c src/parser/python_code_to_c_astdump.c src/parser/python_code_to_c_parser.c \
		src/semantic/python_code_to_c_semantic.c src/codegen/python_code_to_c_codegen.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
		src/platform/python_code_to_c_platform.c $(LDLIBS) -o $(BUILD)/tests/test_heap_unification
	$(BUILD)/tests/test_heap_unification

test-generator-async-runtime:
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_generator_async_runtime.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/test_generator_async_runtime
	$(BUILD)/tests/test_generator_async_runtime

test-async-generator: all
	@mkdir -p build/tests
	$(PYTHON) tests/cases_async_generator_smoke.py > $(BUILD)/tests/async_generator.expected
	./$(BUILD)/python-code-to-c run tests/cases_async_generator_smoke.py > $(BUILD)/tests/async_generator.actual
	diff -u $(BUILD)/tests/async_generator.expected $(BUILD)/tests/async_generator.actual

test-py313-syntax: all
	@mkdir -p build/tests
	./$(BUILD)/python-code-to-c tests/py313_type_params_syntax_alpha10.py -o $(BUILD)/tests/py313_type_params_syntax_alpha10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD)/tests/py313_type_params_syntax_alpha10.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/py313_type_params_syntax_alpha10
	printf '42\n' > $(BUILD)/tests/py313_type_params_syntax_alpha10.expected
	$(BUILD)/tests/py313_type_params_syntax_alpha10 > $(BUILD)/tests/py313_type_params_syntax_alpha10.actual
	diff -u $(BUILD)/tests/py313_type_params_syntax_alpha10.expected $(BUILD)/tests/py313_type_params_syntax_alpha10.actual
	./$(BUILD)/python-code-to-c tests/py313_type_params_extended_syntax_alpha10.py -o $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDLIBS) -o $(BUILD)/tests/py313_type_params_extended_syntax_alpha10
	printf '15\n' > $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.expected
	$(BUILD)/tests/py313_type_params_extended_syntax_alpha10 > $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.actual
	diff -u $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.expected $(BUILD)/tests/py313_type_params_extended_syntax_alpha10.actual

test-baremetal-runtime:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) tests/test_baremetal_runtime.c examples/baremetal/python_code_to_c_baremetal.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c $(LDLIBS) -o $(BUILD)/tests/test_baremetal_runtime
	$(BUILD)/tests/test_baremetal_runtime

# 自作OS統合ファサード(p2c_embed)の回帰: 境界タグ付きヒープ、ライフサイクル、
# GCの安全側停止、OOM通知とMemoryError化。NO_STDLIB + カーネル提供setjmpで、
# Hostedプラットフォームを一切使わずに検証する。
# 組込みランタイムの初期化（組込みモジュール登録）に必要なヒープ量を実測して表示する。
# math モジュールを含む場合と PYTHON_CODE_TO_C_NO_MATH_MODULE で省いた場合の両方を測り、
# HobbyOS向けに必要なヒープ量の目安を示す。
test-embed-baseline:
	@mkdir -p $(BUILD)/tests
	$(CC) -I./include -I./examples/embed -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
		-DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT \
		-ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 -Wall -Wextra \
		tests/measure_embed_baseline.c examples/embed/x86_64_setjmp.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
		src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c \
		$(LDLIBS) -o $(BUILD)/tests/measure_embed_baseline
	$(CC) -I./include -I./examples/embed -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
		-DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT \
		-DPYTHON_CODE_TO_C_NO_MATH_MODULE \
		-ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 -Wall -Wextra \
		tests/measure_embed_baseline.c examples/embed/x86_64_setjmp.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
		src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c \
		$(LDLIBS) -o $(BUILD)/tests/measure_embed_baseline_nomath
	@printf '%s\n' 'with math module:'
	$(BUILD)/tests/measure_embed_baseline
	@printf '%s\n' 'with -DPYTHON_CODE_TO_C_NO_MATH_MODULE:'
	$(BUILD)/tests/measure_embed_baseline_nomath

test-embed-runtime:
	@mkdir -p build/tests
	$(CC) -I./include -I./examples/embed -std=c11 -O2 $(WARN_CFLAGS) -c examples/embed/host_stack_bounds.c -o $(BUILD)/tests/host_stack_bounds.o
	$(CC) -I./include -I./examples/embed -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -DPYTHON_CODE_TO_C_NO_LIBC_STUBS -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) \
		tests/test_embed_runner.c examples/embed/x86_64_setjmp.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c $(BUILD)/tests/host_stack_bounds.o $(LDLIBS) -o $(BUILD)/tests/test_embed_runner
	$(BUILD)/tests/test_embed_runner

# freestanding な setjmp/longjmp 契約（カーネル提供）と例外機構の回帰。
# x86-64 参考実装をリンクし、NO_STDLIB で raise/except/入れ子フレームを検証する。
test-freestanding-setjmp:
	@mkdir -p build/tests
	$(CC) -I./include -I./examples/embed $(HOST_STACK_CFLAGS) -c examples/embed/host_stack_bounds.c -o $(BUILD)/tests/host_stack_bounds.o
	$(CC) -I./include -I./examples/embed $(EMBED_CFLAGS) \
		tests/test_freestanding_setjmp.c examples/embed/x86_64_setjmp.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c $(BUILD)/tests/host_stack_bounds.o $(LDLIBS) -o $(BUILD)/tests/test_freestanding_setjmp
	$(BUILD)/tests/test_freestanding_setjmp

# カーネル相当環境で変換器コア自体を動かす回帰（オンデバイス変換）。
# NO_STDLIB で Python ソース→C 変換が完走し、--embed-entry相当のAPIも動くことを確認する。
test-embed-compile:
	@mkdir -p build/tests
	$(CC) -I./include $(EMBED_CFLAGS) \
		tests/test_embed_transpile.c src/core/python_code_to_c.c src/lexer/python_code_to_c_lexer.c \
		src/parser/python_code_to_c_ast.c src/parser/python_code_to_c_astdump.c src/parser/python_code_to_c_parser.c \
		src/semantic/python_code_to_c_semantic.c src/codegen/python_code_to_c_codegen.c \
		src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
		src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_embed.c $(LDLIBS) -o $(BUILD)/tests/test_embed_transpile
	$(BUILD)/tests/test_embed_transpile

# 改行コード（CRLF/CR/LF）に対して生成Cが完全に一致することを確認する回帰。
# Windowsで保存されたPythonソースが誤変換されないための品質ゲート。
test-crlf: all
	@mkdir -p $(BUILD)/tests
	@P2C_COMPILER="$(BUILD)/python-code-to-c" CC="$(CC)" sh tests/crlf_source_regression.sh

# 自作OSテンプレートのコンパイル検証（警告即エラー）。テンプレートが
# 現行APIから乖離してビルドできなくなることを防ぐ。
test-hobby-os-template: all
	@mkdir -p build/tests
	$(CC) -I./include -I./templates/hobby_os $(EMBED_CFLAGS) -c templates/hobby_os/embed/hobby_os_embed.c -o $(BUILD)/tests/hobby_os_embed.o
	$(CC) -I./include -I./templates/hobby_os $(EMBED_CFLAGS) -c templates/hobby_os/platform/python_code_to_c_platform_user.c -o $(BUILD)/tests/hobby_os_platform_user.o
	$(MAKE) -f templates/hobby_os/hobby_os.mk PROJECT_ROOT=. CC="$(CC)" AR="$(AR)" BUILD="$(BUILD)/hobby_os" clean all
	@test -s $(BUILD)/hobby_os/libpython-code-to-c-hobby-os.a

# 変換済みPythonモジュールをカーネル相当のドライバで実際に走らせる統合テスト。
# --embed-entry で main の無いモジュールを生成し、p2c_embed が提供する
# ヒープ/シンク/スタック境界/panic経路だけで実行してCPythonと差分比較する。
test-embed-generated: all
	@if [ "$$(uname -m)" != "x86_64" ]; then printf '%s\n' 'test-embed-generated: skipped (x86-64 setjmp reference implementation required)'; exit 0; fi
	@mkdir -p build/embed
	./$(BUILD)/python-code-to-c examples/embed/embed_boot.py --embed-entry p2c_embed_program -o $(BUILD)/embed/embed_boot.c
	$(CC) -I./include -I./examples/embed -std=c11 -O2 $(WARN_CFLAGS) -c examples/embed/host_stack_bounds.c -o $(BUILD)/embed/host_stack_bounds.o
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME $(GENERATED_CFLAGS) -c $(BUILD)/embed/embed_boot.c -o $(BUILD)/embed/embed_boot.o
	$(CC) -I./include -I./examples/embed $(EMBED_CFLAGS) -c examples/embed/embed_driver.c -o $(BUILD)/embed/embed_driver.o
	$(CC) $(EMBED_CFLAGS) -c examples/embed/x86_64_setjmp.c -o $(BUILD)/embed/x86_64_setjmp.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/runtime/python_code_to_c_runtime.c -o $(BUILD)/embed/runtime.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/common/python_code_to_c_common.c -o $(BUILD)/embed/common.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/common/python_code_to_c_unicode_tables.c -o $(BUILD)/embed/unicode_tables.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/platform/python_code_to_c_platform.c -o $(BUILD)/embed/platform.o
	$(CC) -I./include $(EMBED_CFLAGS) -c src/platform/python_code_to_c_embed.c -o $(BUILD)/embed/embed.o
	$(CC) $(BUILD)/embed/embed_driver.o $(BUILD)/embed/embed_boot.o $(BUILD)/embed/x86_64_setjmp.o $(BUILD)/embed/runtime.o $(BUILD)/embed/common.o $(BUILD)/embed/unicode_tables.o $(BUILD)/embed/platform.o $(BUILD)/embed/embed.o $(BUILD)/embed/host_stack_bounds.o $(LDLIBS) -o $(BUILD)/embed/embed_boot
	$(PYTHON) examples/embed/embed_boot.py > $(BUILD)/embed/embed_boot.expected
	$(BUILD)/embed/embed_boot > $(BUILD)/embed/embed_boot.actual
	diff -u $(BUILD)/embed/embed_boot.expected $(BUILD)/embed/embed_boot.actual

# ベアメタル構成（自作libcスタブ + libcなし）での例外機構とスタックスキャンの回帰。
# 以前は longjmp スタブが無限ループし、非Linuxではスタック境界が未登録のため
# 自動GCが安全側停止していた。プラットフォームのアロケータが実際に使われることも確認する。
test-baremetal-exceptions:
	@mkdir -p build/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) tests/test_baremetal_exceptions.c examples/baremetal/python_code_to_c_baremetal.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c $(LDLIBS) -o $(BUILD)/tests/test_baremetal_exceptions
	$(BUILD)/tests/test_baremetal_exceptions

# 保守的スタックスキャンが「使用中のスタック範囲」だけを走査することの回帰。
# 以前は宣言区間（Linuxでは既定8MiB）全体を毎回走査し、しきい値を小さくすると
# 「収集回数 × スタック長」で劣化していた。
# ASanはローカル変数をfake stack（実スタック外）へ置くため、保守的スキャンから
# 見えなくなる。走査範囲の意味を保つためdetect_stack_use_after_return=0を付ける。
test-gc-stack-scan-scope:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_stack_scan_scope.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_stack_scan_scope
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_stack_scan_scope

# 式評価中のTLS一時値（binopの左オペランド、処理中の例外、f-stringビルダ）が
# GCのルートに入っていることと、式の途中で例外が脱出したときに深さを巻き戻す
# ことの回帰。生成Cのtryは開始時の深さを保存し、例外ハンドラへ到達した時点で
# p2c_binop_rewind()/p2c_fstr_rewind() で元の深さへ戻す。以前はこれが無く、
#   - 右オペランドの評価中の自動GCが左オペランドを回収し（解放済みポインタを
#     演算関数へ渡す）、
#   - 例外が脱出するたびに一時値が残って64回でnesting limitを誤発火し、
#     f-stringビルダがリークしていた。
# 2つ目（_lsan）はLeakSanitizerでf-stringビルダの解放も確認する。
test-gc-temp-roots:
	@mkdir -p $(BUILD)/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_gc_temp_roots.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(LDFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_temp_roots
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_temp_roots
ifneq ($(IS_TCC),1)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_CFLAGS) tests/test_gc_temp_roots.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/platform/python_code_to_c_gui.c src/modules/python_code_to_c_pygame.c $(SANITIZER_CFLAGS) -pthread $(LDLIBS) -o $(BUILD)/tests/test_gc_temp_roots_lsan
	ASAN_OPTIONS=detect_stack_use_after_return=0:detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 $(BUILD)/tests/test_gc_temp_roots_lsan
endif

# 組込み（カーネル）向けのスタック使用量検査。カーネルスタックは数KiBしか
# 無いことが多く、1フレームが大きい関数はそのままスタックオーバーフローに
# なる。ランタイム本体と変換器コアを -Wstack-usage で検査し、上限超過を
# -Werror で失敗させる。現状の最大値は約3.4KiB（クラス階層のC3線形化と
# クラスオブジェクト生成）で、次点は input() の1.0KiB。
STACK_USAGE_LIMIT ?= 4096
test-stack-usage:
	@$(call gnu_only_gate,test-stack-usage,-Wstack-usage)
	@mkdir -p $(BUILD)/tests
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -Wstack-usage=$(STACK_USAGE_LIMIT) -c src/runtime/python_code_to_c_runtime.c -o $(BUILD)/tests/stack_usage_runtime.o
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -Wstack-usage=$(STACK_USAGE_LIMIT) -c src/common/python_code_to_c_common.c -o $(BUILD)/tests/stack_usage_common.o
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -Wstack-usage=$(STACK_USAGE_LIMIT) -c src/common/python_code_to_c_unicode_tables.c -o $(BUILD)/tests/stack_usage_unicode_tables.o
	@printf '%s\n' "stack_usage_ok: every frame <= $(STACK_USAGE_LIMIT) bytes"

# GCCの静的解析(-fanalyzer)。解放後利用・二重解放・未初期化・確保失敗経路の
# 取り違えなどを関数間で追跡する。実行時間が長いため既定のtestには含めず、
# 明示的に実行する（-Werror で指摘を失敗として扱う）。
# GCCの静的解析をコア全体（lexer/parser/semantic/codegen/common/runtime/platform/
# modules/tools）へ適用する。以前はcore.cとruntime.cの2ファイルだけだった。
ANALYZER_SRCS := src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
	src/lexer/python_code_to_c_lexer.c \
	src/parser/python_code_to_c_ast.c \
	src/parser/python_code_to_c_astdump.c \
	src/parser/python_code_to_c_parser.c \
	src/semantic/python_code_to_c_semantic.c \
	src/codegen/python_code_to_c_codegen.c \
	src/core/python_code_to_c.c \
	src/runtime/python_code_to_c_runtime.c \
	src/platform/python_code_to_c_platform.c \
	src/platform/python_code_to_c_platform_hosted.c \
	src/platform/python_code_to_c_embed.c \
	src/platform/python_code_to_c_gui.c \
	src/modules/python_code_to_c_pygame.c \
	src/tools/python_code_to_c_httpd.c \
	src/tools/python_code_to_c_gui_web.c \
	src/tools/c2py.c

# 組込み（自作OS/ベアメタル）へ実際に載る「コア」だけを対象にしたスタック予算の
# 検証用ソース一覧。ホスト側ツール（httpd/gui_web/c2py 等）は含めない
# （実測最大はツールの 8304 バイト、コアは 3456 バイト）。
CORE_STACK_SRCS := src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
	src/lexer/python_code_to_c_lexer.c \
	src/parser/python_code_to_c_ast.c \
	src/parser/python_code_to_c_astdump.c \
	src/parser/python_code_to_c_parser.c \
	src/semantic/python_code_to_c_semantic.c \
	src/codegen/python_code_to_c_codegen.c \
	src/core/python_code_to_c.c \
	src/runtime/python_code_to_c_runtime.c \
	src/platform/python_code_to_c_platform.c \
	src/platform/python_code_to_c_embed.c \
	src/modules/python_code_to_c_pygame.c

# 変換器そのもの（lexer/parser/semantic/codegen）を ASan+UBSan でビルドし、
# 全フィクスチャを strict と --fallback の両方で変換して一巡させる回帰。
# ランタイム中心の test-sanitizers と相補的（こちらは変換時の未定義動作を検出）。
test-sanitizers-core:
	@$(call gnu_only_gate,test-sanitizers-core,-fsanitize)
	@mkdir -p build/tests
	$(MAKE) BUILD=build/sanitize-core OBJ=obj/sanitize-core \
		CFLAGS="$(CFLAGS) $(SANITIZER_CFLAGS)" LDFLAGS="$(LDFLAGS) $(SANITIZER_CFLAGS)" all
	$(SANITIZER_ENV) sh tests/sanitize_pipeline_alpha10.sh build/sanitize-core/python-code-to-c

test-analyzer:
	@$(call gnu_only_gate,test-analyzer,-fanalyzer)
	@mkdir -p $(BUILD)/tests
	@for src in $(ANALYZER_SRCS); do \
		out=$(BUILD)/tests/analyzer_$$(basename $$src .c).o; \
		$(CC) -I./include -std=c11 -O0 $(WARN_CFLAGS) -fanalyzer \
			-Wno-analyzer-too-complex -c $$src -o $$out || exit 1; \
	done
	@printf '%s\n' 'analyzer_ok: -fanalyzer reported no defects across all sources'

# clang の静的解析器によるクロスチェック（clangがある環境のみ）。
# GCC -fanalyzer とは別実装のため、片方だけが見つける欠陥を拾える。
CLANG ?= clang
CLANG_ANALYZE_SRCS := $(ANALYZER_SRCS)

test-analyzer-clang:
	@$(call gnu_only_gate,test-analyzer-clang,clang --analyze)
	@command -v $(firstword $(CLANG)) >/dev/null || { printf '%s\n' "clang not found: $(CLANG) (optional cross-check)" >&2; exit 1; }
	@mkdir -p $(BUILD)/tests
	@for src in $(CLANG_ANALYZE_SRCS); do \
		$(CLANG) --analyze -Xanalyzer -analyzer-output=text -I./include -std=c11 -O0 \
			-Wno-everything -o /dev/null $$src || exit 1; \
	done
	@printf '%s\n' 'analyzer_clang_ok: clang static analyzer reported no defects'

# 性能ヒント系のみを集めた任意ターゲット（正しさの検査ではないため既定から分離）。
#   -Wsuggest-attribute=const,pure : 引数以外に依存しない関数へ属性を促し、
#                                    不要な再計算・再読み出しを減らす。
#   -Wunsafe-loop-optimizations    : 最適化できない（反復回数が確定しない）ループを
#                                    指摘し、ホットパスの見直し材料にする。
#   -Wstack-usage は既定（WARN_CFLAGS）に含まれ、組込みコアの厳しい予算は
#   make test-stack-budget が担う（ここは性能ヒントの読み物なので重ねて課さない）。
# ヒント系は「助言」なので -Wno-error を付け、違反しても失敗させない（人が読んで
# 判断するためのレポート）。
OPT_HINT_CFLAGS ?= -Wsuggest-attribute=const -Wsuggest-attribute=pure \
	-Wunsafe-loop-optimizations \
	-Wno-error=suggest-attribute=const -Wno-error=suggest-attribute=pure \
	-Wno-error=unsafe-loop-optimizations

test-opt-hints:
	@$(call gnu_only_gate,test-opt-hints,-Wsuggest-attribute)
	@mkdir -p $(BUILD)/tests
	@for src in $(ANALYZER_SRCS); do \
		out=$(BUILD)/tests/opthint_$$(basename $$src .c).o; \
		$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) $(OPT_HINT_CFLAGS) \
			-c $$src -o $$out || exit 1; \
	done
	@printf '%s\n' 'opt_hints_ok: performance hints listed above (advisory; build not failed)'

# 組込み向けスタック予算の回帰ゲート。組込みコア（ホスト用ツールを除く）を
# 実測最大 3456 バイトより少し厳しい 4096 バイトでビルドし、余裕があることを数値で
# 担保する。大きな配列をスタックに置く変更はここで失敗する。
test-stack-budget:
	@$(call gnu_only_gate,test-stack-budget,-Wstack-usage)
	@for src in $(CORE_STACK_SRCS); do \
		out=$(BUILD)/tests/stackbudget_$$(basename $$src .c).o; \
		mkdir -p $(BUILD)/tests; \
		$(CC) -I./include -std=c11 -O2 $(WARN_CFLAGS) -Wstack-usage=4096 \
			-c $$src -o $$out || exit 1; \
	done
	@printf '%s\n' 'stack_budget_ok: embedded core fits in 4096 bytes of stack per frame'

test-baremetal-build:
	$(MAKE) -f templates/toolchains/baremetal-example.mk PROJECT_ROOT=. CC="$(CC)" AR="$(AR)" BUILD="$(BUILD)/baremetal" clean all
	test -s $(BUILD)/baremetal/libpython-code-to-c-baremetal-example.a

test-baremetal-generated: all
	@mkdir -p build/baremetal
	./$(BUILD)/python-code-to-c examples/baremetal/baremetal_hello.py -o $(BUILD)/baremetal/baremetal_hello.generated.c
	$(CC) -I./include -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 $(WARN_CFLAGS) -c $(BUILD)/baremetal/baremetal_hello.generated.c -o $(BUILD)/baremetal/baremetal_hello.generated.o

# ── C99 / TinyCC 構成 (Beta0.1) ───────────────────────────────────────────
# 既定ビルドはC11＋厳格警告（-Wconversion等）のまま維持し、C99専用コンパイラ
# （TinyCC等）向けのビルド経路を別ターゲットとして用意する。C99では使えない
# 厳格警告はC99構成では有効化せず、C99で意味のある最小限
# （-Wall -Wextra -Werror -pedantic）に留める。C11側の厳格基準は不変なので、
# 品質ゲートが緩むことはない。
C99_CFLAGS ?= -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pedantic
C99_BUILD ?= build/c99
C99_OBJ ?= obj/c99
# 生成Cは内包表記でGNU statement expressionを使うため-pedanticは付けない
# （--c11 は変換時にその構文を拒否する）。C99コンパイラでも受理される。
C99_GENERATED_CFLAGS ?= -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
TCC ?= tcc
# TinyCC が解釈できる警告だけを有効化する。tcc は -Wconversion 等のGCC固有の
# 厳格警告を持たず、追加の -W オプション（-Wshadow 等）は「実ソースによっては
# unsupported option」で落ちることがあるため、実績のある集合に留める
# （C11厳格ビルドの基準はWARN_CFLAGS側で維持する）。
TCC_CFLAGS ?= -std=c99 -O2 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Werror \
	-Wunsupported -Wwrite-strings
TCC_BUILD ?= build/tcc
TCC_OBJ ?= obj/tcc
ELF_TCC_BUILD ?= build/elf-tcc
HOBBYOS_TCC_BUILD ?= build/hobbyos-tcc
# TinyCC で NO_STDLIB（カーネル/組込み）構成のソースをコンパイルできることの確認用。
# -Wunsupported は付けない: カーネル向けテンプレートが使うアセンブリのディレクティブ
# （.size 等）をTinyCCが「未対応」として警告し、-Werrorで落ちるため。
TCC_FREESTANDING_CFLAGS ?= -std=c99 -O2 -Wall -Werror -Wwrite-strings \
	-I./include -I./templates/hobby_os -I./examples/embed \
	-DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
	-DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT

c99:
	$(MAKE) CC="$(CC)" BUILD="$(C99_BUILD)" OBJ="$(C99_OBJ)" CFLAGS="$(C99_CFLAGS)" all gui
	@printf '%s\n' "c99_build_ok: $(C99_BUILD)/python-code-to-c $(C99_BUILD)/python-code-to-c-gui"
check-tcc:
	@command -v $(firstword $(TCC)) >/dev/null || { printf '%s\n' "TinyCC not found: $(firstword $(TCC)) (install tcc, or pass TCC=/path/to/tcc)" >&2; exit 1; }
	@printf '%s\n' "toolchain_ok: TCC=$(TCC)"

tcc: check-tcc
	$(MAKE) CC="$(TCC)" BUILD="$(TCC_BUILD)" OBJ="$(TCC_OBJ)" DEPFLAGS= CFLAGS="$(TCC_CFLAGS)" all gui
	@printf '%s\n' "tcc_build_ok: $(TCC_BUILD)/python-code-to-c $(TCC_BUILD)/python-code-to-c-gui"

# C99でビルドした変換器で、CPython差分コーパス全体（528アサーション）を変換し、
# 生成CもC99としてコンパイル・実行して一致を確認する。
test-c99: c99
	P2C_COMPILER=$(C99_BUILD)/python-code-to-c P2C_TEST_CFLAGS="$(C99_GENERATED_CFLAGS)" CC="$(CC)" sh tests/conformance_regression.sh

# TinyCCでカーネル/組込み構成（NO_STDLIB）のソースもコンパイルできることを確認する。
# 生成物はリンクしない（カーネル側の実装をリンクする前提のため、コンパイル検証まで）。
test-tcc-freestanding: check-tcc
	@mkdir -p $(TCC_BUILD)/freestanding
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c src/common/python_code_to_c_common.c \
		-o $(TCC_BUILD)/freestanding/common.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c src/common/python_code_to_c_unicode_tables.c \
		-o $(TCC_BUILD)/freestanding/unicode_tables.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c src/runtime/python_code_to_c_runtime.c \
		-o $(TCC_BUILD)/freestanding/runtime.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c src/platform/python_code_to_c_platform.c \
		-o $(TCC_BUILD)/freestanding/platform.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c src/platform/python_code_to_c_embed.c \
		-o $(TCC_BUILD)/freestanding/embed.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c templates/hobby_os/embed/hobby_os_libc.c \
		-o $(TCC_BUILD)/freestanding/hobbyos_libc.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c examples/embed/x86_64_setjmp.c \
		-o $(TCC_BUILD)/freestanding/x86_64_setjmp.o
	$(TCC) $(TCC_FREESTANDING_CFLAGS) -c templates/hobby_os/embed/hobby_os_entry.c \
		-o $(TCC_BUILD)/freestanding/hobbyos_entry.o
	@printf '%s\n' 'tcc_freestanding_ok: NO_STDLIB core, templates and setjmp compiled with TinyCC'

# TinyCCで変換器本体と生成Cの両方をビルドして同じ差分コーパスを通す。単一ヘッダーも確認する。
test-tcc: tcc test-single-header-tcc test-tcc-freestanding
	P2C_COMPILER=$(TCC_BUILD)/python-code-to-c P2C_TEST_CFLAGS="-std=c99 -Wall -D_POSIX_C_SOURCE=200809L" CC="$(TCC)" sh tests/conformance_regression.sh

# ── ELF 生成 ─────────────────────────────────────────────────────────────────
# ELF の検査はすべて tools/p2c_elfcheck.c（binutils 非依存）で行う。
#   make elf-full        : GCC/Clang の静的 CLI ELF（BUILD/elf）
#   make tcc-elf         : TinyCC の CLI ELF（動的リンク。TinyCC は glibc を静的リンクできない）
#   make hobbyos elf     : HobbyOS 向け ELF（libc なし・静的・W^X・未定義シンボル無し）
#   make tcc hobby c99   : TinyCC 版の HobbyOS ELF（ENTRY は _start の薄いシム）
# 種別（CC=tcc か、goal に tcc があるか）で経路を選ぶ。
ELF_BUILD ?= build/elf
ELF_CFLAGS ?= -static -fno-pie -no-pie
ELF_LDFLAGS ?= -static -no-pie
ELF_TCC_BUILD ?= build/elf-tcc
HOBBYOS_ELF_BUILD ?= build/hobbyos
HOBBYOS_TCC_BUILD ?= build/hobbyos-tcc
HOBBYOS_ELF_PROGRAM ?= examples/embed/embed_boot.py
HOBBYOS_ELF_ENTRY ?= p2c_hobbyos_program
# HobbyOS のロードアドレス（空なら templates/hobby_os/hobbyos.ld の既定 0x400000）。
# hobbyos.ld は DEFINED(HOBBYOS_LOAD_ADDR) でリンカ変数を見るため、-D（前処理器の値）
# ではなく --defsym で渡す。TinyCC はリンカスクリプトを扱えないので、この値は GCC 経路のみ。
HOBBYOS_LOAD_ADDR ?=
comma := ,
HOBBYOS_LOAD_LDFLAGS := $(if $(HOBBYOS_LOAD_ADDR),-Wl$(comma)--defsym=HOBBYOS_LOAD_ADDR=$(HOBBYOS_LOAD_ADDR),)
HOBBYOS_ELF_LDFLAGS ?= -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-e,p2c_hobbyos_entry -T templates/hobby_os/hobbyos.ld -lgcc
TCC_OBJFILES := $(patsubst src/%.c,$(TCC_OBJ)/%.o,$(SRC))

ELF_USE_TCC := $(or $(IS_TCC),$(filter tcc,$(MAKECMDGOALS)))
ELF_HOBBY := $(filter hobby hobbyos,$(MAKECMDGOALS))
ifneq ($(ELF_USE_TCC),)
ELF_TARGET := $(if $(ELF_HOBBY),tcc-hobbyos-elf,tcc-elf)
else
ELF_TARGET := $(if $(ELF_HOBBY),hobbyos-elf,elf-full)
endif

elf: $(ELF_TARGET)

$(ELFCHECK): tools/p2c_elfcheck.c
	@mkdir -p $(@D)
	$(CC) $(ELFCHECK_CFLAGS) $< -o $@

elfcheck: $(ELFCHECK)
	@printf '%s\n' "elfcheck_ok: $(ELFCHECK)"

elf-full: $(if $(IS_TCC),tcc-elf,all $(ELFCHECK))
ifneq ($(IS_TCC),1)
	@mkdir -p $(ELF_BUILD)
	$(CC) $(CFLAGS) $(ELF_CFLAGS) $(LDFLAGS) $(OBJFILES) $(LDLIBS) -o $(ELF_BUILD)/python-code-to-c.elf
	@cp -f $(ELF_BUILD)/python-code-to-c.elf $(ELF_BUILD)/python-code-to-c
	@$(ELFCHECK) --exec --static $(ELF_BUILD)/python-code-to-c.elf > $(ELF_BUILD)/python-code-to-c.elf.txt; rc=$$?; cat $(ELF_BUILD)/python-code-to-c.elf.txt; exit $$rc
	@$(ELF_BUILD)/python-code-to-c.elf --supported | head -1
	@printf '%s\n' "elf_full_ok: $(ELF_BUILD)/python-code-to-c.elf (static, manifest=$(ELF_BUILD)/python-code-to-c.elf.txt)"
endif

tcc-elf: tcc
	$(MAKE) CC="$(TCC)" BUILD="$(TCC_BUILD)" OBJ="$(TCC_OBJ)" DEPFLAGS= CFLAGS="$(TCC_CFLAGS)" elfcheck
	TCC="$(TCC)" P2C_ELFCHECK="$(TCC_BUILD)/tools/p2c_elfcheck" ELF_BUILD="$(ELF_TCC_BUILD)" P2C_OBJS="$(TCC_OBJFILES)" sh tests/build_tcc_elf_alpha10.sh full

# TinyCC だけで全工程を実行する（GCC/Clang/binutils 不要）。
# スレッド別の回帰（test-threads）は TinyCC では対象外: TinyCC は P2C_THREAD_LOCAL を持たない。
tcc-full: check-tcc
	$(MAKE) tcc
	$(MAKE) test-tcc
	$(MAKE) tcc-elf
	$(MAKE) tcc-hobbyos-elf
	@printf '%s\n' 'tcc_full_ok: TinyCC build, C99 corpus, freestanding, CLI ELF, HobbyOS ELF'

tcc-hobbyos-elf: tcc
	$(MAKE) CC="$(TCC)" BUILD="$(TCC_BUILD)" OBJ="$(TCC_OBJ)" DEPFLAGS= CFLAGS="$(TCC_CFLAGS)" elfcheck
	@mkdir -p $(HOBBYOS_TCC_BUILD)
	./$(TCC_BUILD)/python-code-to-c $(HOBBYOS_ELF_PROGRAM) --embed-entry $(HOBBYOS_ELF_ENTRY) -o $(HOBBYOS_TCC_BUILD)/program.c
	TCC="$(TCC)" P2C_ELFCHECK="$(TCC_BUILD)/tools/p2c_elfcheck" HOBBYOS_ELF_BUILD="$(HOBBYOS_TCC_BUILD)" PROGRAM_C="$(HOBBYOS_TCC_BUILD)/program.c" sh tests/build_tcc_elf_alpha10.sh hobbyos

hobby:
	@printf '%s\n' 'hobby は hobbyos の別名です（`make tcc elf hobby c99` などで使用）。'

hobbyos:
	@printf '%s\n' 'hobbyos: `make hobbyos elf`（または `make hobbyos-elf`）でHobbyOS向けELFをビルドします。'

hobbyos-elf: $(if $(IS_TCC),tcc-hobbyos-elf,all $(ELFCHECK))
ifneq ($(IS_TCC),1)
	@mkdir -p $(HOBBYOS_ELF_BUILD)
	./$(BUILD)/python-code-to-c $(HOBBYOS_ELF_PROGRAM) --embed-entry $(HOBBYOS_ELF_ENTRY) -o $(HOBBYOS_ELF_BUILD)/program.c
	$(CC) -I./include -I./templates/hobby_os -ffreestanding -fno-builtin -fno-stack-protector -std=c11 -O2 \
		-DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME \
		-DP2C_EMBED_PROVIDE_PLATFORM_COMPAT \
		-Wall -Wextra -Werror \
		$(HOBBYOS_ELF_BUILD)/program.c \
		src/runtime/python_code_to_c_runtime.c \
		src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
		src/platform/python_code_to_c_platform.c \
		src/platform/python_code_to_c_embed.c \
		templates/hobby_os/embed/hobby_os_entry.c \
		templates/hobby_os/embed/hobby_os_libc.c \
		$(HOBBYOS_LOAD_LDFLAGS) $(HOBBYOS_ELF_LDFLAGS) -o $(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf
	@$(ELFCHECK) --exec --static --no-undef --entry-sym p2c_hobbyos_entry $(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf > $(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf.txt; rc=$$?; cat $(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf.txt; exit $$rc
	@printf '%s\n' "hobbyos_elf_ok: $(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf (elf_entry=p2c_hobbyos_entry, program=$(HOBBYOS_ELF_ENTRY), manifest=$(HOBBYOS_ELF_BUILD)/python-code-to-c-hobbyos.elf.txt)"
endif

# HobbyOS 向け ELF が要求する数学・書式化スタブ（templates/hobby_os/embed/hobby_os_libc.c）を
# ホストの libm/libc と比較して検証する。シンボルは -include で stub_ 前置へ改名する。
test-hobbyos-libc:
	@mkdir -p $(BUILD)/tests
	$(CC) -I./include -std=c11 -O2 -Wall -Wextra -Werror \
		-DP2C_HOBBYOS_LIBC_PREFIX \
		templates/hobby_os/embed/hobby_os_libc.c tests/test_hobbyos_libc.c \
		$(LDLIBS) -o $(BUILD)/tests/test_hobbyos_libc
	$(BUILD)/tests/test_hobbyos_libc

install: all
	install -Dm755 $(BUILD)/python-code-to-c $(DESTDIR)/usr/local/bin/python-code-to-c

clean:
	rm -rf $(BUILD) $(OBJ)

clean-all:
	rm -rf build obj

-include $(DEPS)
