# Sprint — 軽くて速くて隠し事のないスクリプト言語

C11 で書かれたスタックVM型インタプリタと、その言語仕様（`SPEC.md` が目次、本文は `docs/spec/`。日本語・規範文書）です。

- **隠れた制御フローなし / 隠れたメモリ割り当てなし**: 暗黙のコード実行・暗黙の割り当てが起きる場所は仕様の付録A・Bに列挙したものだけ。
- **堅牢なビルド**: `-Werror` と約70の警告、hardening、ASan+UBSan、`-fanalyzer` をすべて通ります。
- **単一バイナリ・依存なし**（libc と libm のみ）。

## ビルドとテスト

```sh
make            # 厳格リリースビルド → build/sprint
make test       # 回帰テスト（stdout と終了コードを比較）
make debug && make test-san   # ASan + UBSan 版で同じテスト
make analyze    # GCC 静的解析 (-fanalyzer)
make fuzz       # .spc ローダ/verifier/VM の変異ファジング（ASan+UBSan）
make lib        # 埋め込み用 build/libsprint.a（ヘッダ src/sprint.h。リンクは -lm -ldl）
make test-embed # 埋め込みAPIのテスト（通常版＋ASan/UBSan版）とサンプル
```

## 使い方

```sh
build/sprint run program.sp [引数...]
build/sprint check program.sp          # 構文・名前解決だけ検査
build/sprint disasm program.sp         # バイトコード表示
build/sprint compile -o out.spc program.sp   # program image (.spc) を出力（決定的）
build/sprint exec out.spc [引数...]    # imageを検証してから実行（`run`でも自動判別）
build/sprint run --json program.sp     # 診断・未処理エラーをJSONで
build/sprint repl                      # 対話環境（:help :names :reset :quit、直前の値は _）
build/sprint fmt [--check] [-w] FILE... # 整形（トークン同値を検査してから出力）
build/sprint lint [--json] FILE...     # L001-L006 の警告（未使用・到達不能・重複key など）
```

実行時オプション: `--deterministic` / `--max-heap N` / `--max-instr N` /
`--max-runtime-ms N` / `--allow-read=DIR` / `--allow-write=DIR`（fs・gfxの保存）/ `--allow-random` / `--allow-ffi` / `--deny-clock` / `--allow-all`（ffi は含まない）。
権限が無い操作は PermissionError（終了コード 3）になります。

終了コード: 0 正常 / 1 実行時エラー / 2 使い方 / 3 権限 / 4 import失敗 / 12 コンパイルエラー / 13 imageのロード失敗 / 14 資源上限 / 101 panic / 124 タイムアウト / 130 中断（`sp_interrupt`）。

## 30秒で分かる Sprint

```sprint
import math

class Counter:
    let n = 0
    func init(self, start):
        self.n = start
    func bump(self):
        self.n += 1
        return self.n

let c = Counter(5)
for i in range(3):
    c.bump()
print(c.n)                                   # 8

try:
    throw error("ValueError", "negative")
catch e:
    print(e.kind, e.message)

print([1, 2, 3].map(func(x): x * x))         # [1, 4, 9]

func work():
    defer print("cleanup")                   # 関数を抜ける時に必ず実行
    return 1
```

2.1 で追加した構文:

```sprint
let [first, second, ...rest] = [1, 2, 3, 4]     # 分割代入
print(f"{first}+{second} = {first + second}")    # f-string（{{ }} で波括弧）
print([10, 20, 30, 40][1:3], nil ?? "既定値")     # スライス糖衣 / nil合体
let n = 5
n **= 2                                          # 追加の複合代入（//= <<= など）
```

2.2 で追加した構文（すべて既存構文への糖衣で、2.1 のプログラムはそのまま動きます）:

```sprint
let sq = x => x * x                              # アロー関数（(a, b) => a + b、() => 1）
let ok = [1, 2, 3, 4]
    |> filter_odd                                # パイプ x |> f(a) は f(x, a)。行頭の |> は継続行
    |> scale(10)
print([x * 2 for x in range(10) if x % 3 == 0])  # 内包表記（二重の for も可）
print({k: len(k) for k in ["a", "bcd"]})         # table内包表記（キーは式）
for x in [1, 2, 3, 4] if x % 2 == 0:             # for ... if: 本体を if で包む糖衣
    print(sq(x))
let size = match 7:                               # match式（腕は case p => e）
    case 0 => "zero"
    case n if n < 10 => "small"
    case _ => "big"
```

2.3 で追加した構文（これも糖衣で、2.2 のプログラムはそのまま動きます）:

```sprint
print(3 in [1, 2, 3], "ab" in "cabd", "k" not in {j: 1})   # in / not in（配列・文字列・table）
if 0 < n <= 10:                                  # 連鎖比較（中間の項は1回だけ評価）
    print("in range")
let city = user?.address?.city ?? "unknown"      # ?. ?[ ] は nil なら後続を飛ばして nil
cache.hits ??= 0                                 # nil のときだけ代入
a, b = b, a                                      # 多重代入（入れ替え）。x, y = pair も可
let point = {x, y}                               # table省略形 = {x: x, y: y}
for [name, score] in pairs:                      # for の分割代入（{x, y: z} も可）
    print(name, score)
if let found = lookup("k"):                      # nil でなければ束縛して実行
    print(found)
while let job = next_job():                      # nil になるまで繰り返す
    run(job)
print(r"C:\new\table")                           # 生文字列（エスケープなし）
```

`examples/` に動く例があります（`basics/` の `tour.sp` `wordcount.sp` `generator.sp` `json_roundtrip.sp`、`gfx/gfx_demo.sp`、`gui/` の `gui_demo.sp` `gui_layout.sp` `gui_workbench.sp` `gui_studio.sp` など、埋め込みは `examples/embed/host.c`）。

## 言語の要点

| 項目 | 内容 |
|---|---|
| 構文 | `:` + インデント。コメントは `#`。`let` は可変、`const` は不変 |
| 型 | nil, bool, int(64bit), float, string(UTF-8), array, table, function, class, instance, module, iterator, coroutine, error |
| 制御 | `if/elif/else`, `while`, `for x in ...`（array, table, iterator, coroutine）, `match`, ラベル付き `break/continue` |
| 関数 | first-class、closure、default/rest引数、スプレッド呼出し、lambda `func(x): expr` / `x => expr`、パイプ `\|>`、内包表記 |
| class | 単一継承（`extends`。別moduleのclassも継承できる: `class B extends m.A`）、`override`、`super`、フィールドは宣言必須 |
| エラー | `throw` / `try-catch`、`defer`、`panic`（捕捉不可）、`assert` |
| 並行 | stackful `coroutine`（`yield`、`coroutine.resume/close`） |
| module | `import a.b`、`from a.b import x as y`、`export`。循環・未発見は実行前に ImportError |

## 標準ライブラリ

- **prelude**: `print len type to_string to_int to_float range error error_trace has delete deep_equal classof class_name is_instance`
- **gui**: 同梱module（Sprintで書かれバイナリに埋め込み）。ウィジェットとイベントループに加え、layout engine（`column`/`row`/`grid`/flex/`scroll`）、theme切替（dark/light）、仮想時計の`animate`、イベントの記録/再生、スナップショットhash検査、Tabs/Dropdown/TextArea/MenuBar/Tree/Table/Splitter/Spinner/RadioGroup/右クリックメニュー/tooltip/toast/modal dialog。ウィンドウは持たず、合成イベントで駆動して `draw()` で gfx canvas（PNG出力）に描く決定的なheadless GUI。`examples/gui/` 参照
- **gui の入力と編集**（2.4）: テキスト編集（選択・clipboard・undo/redo・単語移動・ダブル/トリプルクリック）、`mods`/`clicks` つきのイベント、`app.bind("Ctrl+S", fn)`、スクロールバー、折り返しLabel、`Flow`/`Stack`/`Expander`、ドラッグ＆ドロップ
- **guiplus**（2.4）: 描画中心のウィジェット集（`gui.Widget` を別moduleから継承）。`chart`（line/area/bar/scatter/pie/donut、凡例・格子・ホバー表示・選択）、`canvas_view`（お絵描き面）、`color_picker`（HSV）、`calendar`、アイコン19種・`toolbar`・`StatusBar`。整数演算のみで決定的。`examples/gui/gui_studio.sp` 参照
- **window / vfs / proc / wm**（2.6）: Sprint製の同梱module。ウィンドウのバックエンド層（headless同梱・ネイティブは未同梱）、メモリ上のファイルシステム、協調マルチタスクのカーネルとシェル、ウィンドウ/タスクバー/ターミナル/ポインタを持つデスクトップ。`examples/os/minios.sp` 参照。
- **gui の当たり判定とポインタ**（2.6）: `shape_ellipse/round/polygon/mask`、`set_through`、`Widget.pointer`、`App.show_cursor`（ソフトウェアポインタ）。
- **itertools / strutil**: 同梱module（Sprintで書かれバイナリに埋め込み）。`itertools` は `enumerate` `zip` `take`/`drop` `chunks` `windows` `flatten` `unique` `group_by` `partition` `sum` `min_by`/`max_by` `sort_by` など、`strutil` は `pad_left`/`pad_right`/`center` `title` `words` `truncate` `wrap` `indent` `reverse` など
- **ffi**: 同梱module。Cの共有ライブラリ（`extern "C"` のC++も）を `ffi.open(nil).fn("sqrt", "double sqrt(double)")(2.0)`（または `ffi.cdef` で宣言して `bind`）のように呼べる。構造体・配列・コールバック（Sprint関数をqsortなどへ渡す）に対応。型安全の外にあるため `--allow-ffi` が必要で `--allow-all` には含まれず、`--deterministic` では使えない。x86-64 / AArch64 のみ（整数6・浮動小数8引数まで。構造体の値渡し/返却は x86-64 で16バイト以下＋大きい構造体の返却に対応）。`examples/ffi/ffi_demo.sp` 参照
- **gfx**: ソフトウェア描画canvas（整数のみの決定的ラスタライザ、5x7フォント、clip・角丸・グラデーション・AA線・`hash`/`diff`）。`c.save("out.png")` で PNG/PPM/BMP を出力（`--allow-write` 要）
- **math** / **str** / **table** / **io** / **sys** / **time** / **gc** / **coroutine**
- **json**: `json.parse(s)`, `json.stringify(v, indent=0)`（深さ512、RFC 8259）
- **random**: xoshiro256** + SplitMix64（仕様で固定。`random.seed(n)` 後は全処理系で同じ列）
- **fs**: `read_file write_file(atomic) append_file exists is_file is_dir list_dir make_dir remove`（`--allow-*` が必要、エラーの `data` に `{errno, op, path}`）
- **string メソッド**: `slice find contains starts_with ends_with count split split_lines trim upper lower replace repeat char_count char_at is_boundary codepoints`
- **array メソッド**: `append extend insert pop remove_at clear slice copy index_of contains reverse join sort each map filter reduce any all find`

## ソース構成

ソースは段階（カテゴリ）ごとのディレクトリに分けてあります。大きな翻訳単位は役割で分割し、
同じ段階の中で共有する関数だけを `*_int.h`（内部ヘッダ）に出しています（`vm.c` の解釈ループは
インライン展開を保つため1ファイルのままです）。

| ディレクトリ / ファイル | 役割 |
|---|---|
| `src/sp.h` `src/sprint.h` | 内部の共通ヘッダ / 公開のC埋め込みAPIヘッダ（SPEC §20） |
| `src/front/lexer.c` | 字句解析（INDENT/DEDENT） |
| `src/front/parser.c` `parse_expr.c` `parser_int.h` | 構文解析（arena AST）。式は `parse_expr.c` |
| `src/front/compiler.c` `compiler_int.h` | 2パスの駆動、関数・module・REPLスコープ |
| `src/front/cmp_util.c` `cmp_expr.c` `cmp_stmt.c` `cmp_class.c` `cmp_import.c` | 命令出力・名前解決 / 式 / 文 / class（別moduleから継承される情報も） / import・export |
| `src/front/disasm.c` `loader.c` | 逆アセンブラ / module探索・循環検出・依存先優先のコンパイル |
| `src/runtime/vm.c` `vm_core.c` | 解釈ループ（defer・例外・coroutine・native再入） / VM生成・エラー構築 |
| `src/runtime/value.c` `table.c` `format.c` `gc.c` | オブジェクト・文字列・array / 順序table / 文字列化と標準出力 / GC（mark-sweep） |
| `src/runtime/image.c` `sha256.c` `verify.c` | program image (.spc) の符号化・復号 / SHA-256 / verifier（命令境界・stack高さ・handler深さ等） |
| `src/runtime/embed.c` | C埋め込みAPI（SPEC §20）。host module・native object・コールバック・割り込み |
| `src/lib/lib.c` `lib_math.c` `lib_str.c` `lib_array.c` `lib_table.c` `lib_sys.c` `lib_co.c` | prelude・module登録 / math / str と文字列メソッド / arrayメソッド / table / io・sys・time・gc / coroutine |
| `src/lib/lib_ext.c` `lib_gfx.c` `lib_ffi.c` | fs・json・random など / gfx（canvas と PNG エンコーダ） / FFI（dlopen・レジスタ呼出し・コールバック） |
| `stdlib/*.sp` | 同梱module（guiplus・ffi・itertools・strutil・window・vfs・proc・wm）。ビルド時にCソースへ埋め込む（`tools/embedsrc.c`） |
| `stdlib/gui/NN_*.sp` | `gui` を役割ごとに分けた部品（core / basic / text / choice / menu / data / controls / layout / factories / app）。ビルド時に番号順に連結して1つのmodule `gui` として埋め込む（実行時のエラー行番号は連結後の位置） |
| `cli/` | CLI（`main.c`）、`tool_fmt.c`（fmt）、`tool_lint.c`（lint）、`repl.c` |
| `tests/cases/<分類>/` | 回帰テスト（`lang` `syntax` `class` `limits` `stdlib` `gfx` `gui`。`.sp` + `.out`/`.rc`/`.args`）。`tests/mods/*` は複数ファイル、`tests/fmt` `lint` `repl` `ffi` `embed` はツール別 |
| `examples/{basics,gfx,gui,ffi,embed}/` | 動く例（期待出力は `tests/examples/*.out`） |
| `docs/spec/` | 言語仕様（`SPEC.md` が目次。章ごとのファイル、§15 標準ライブラリは節ごと） |
| `tools/` | `embedsrc.c`、`fuzz_image.c`（imageの変異ファザ）、`bench/` |

## 速度

インタプリタは検証済みバイトコードを実行直前に「融合命令」へ最適化します（`LOAD_LOCAL+SMALLINT+ADD`、
`比較+JUMP_IF_FALSE`、`RANGE_NEXT+STORE_LOCAL` など。先頭1バイトだけを書き換えるので、命令長・行番号・
`.spc` イメージ・エラー位置は変わりません。整数でない/オーバーフローする場合は元の命令列に戻って実行します）。
実行制限 `--max-instr` は「後方jumpと呼出しの回数」を数えるステップ予算で、命令ごとの計数コストがありません。

同一マシン・同一ベンチでの実測（秒、小さいほど速い）:

| ベンチ | 最適化前 | 最適化後 |
|---|---|---|
| fib(35) 再帰呼出し | 1.95 | 0.49 |
| 1億回ループ | 6.75 | 1.35 |
| 300万回 クラス/メソッド | 1.58 | 0.78 |
| クロージャ2千万回 | 1.29 | 0.57 |
| 配列 300万要素 | 0.46 | 0.23 |
| テーブル+文字列キー | 0.81 | 0.41 |
| 文字列 join/split | 0.30 | 0.17 |

主な最適化: 融合命令（`for`/`while` の頭をジャンプ側でインライン実行、`self.x`、`a[i]`、`return x` なども融合。`--no-quicken` で無効化でき、テストは全ケースで有効/無効の出力一致を確認） / ステップ予算 / 呼出し・return の高速パス / 配列添字・メンバ参照の高速パスと
(クラス,名前)インラインキャッシュ（GCごとに破棄） / 小オブジェクトのサイズクラスプール /
組込みメソッドのハッシュ引き / 整数→文字列とCONCATの自前実装 / 32bit除算 /
`SpValue` を2語ストアで構築（ストアフォワーディング失敗の回避。x86では `-mtune-ctrl` も併用）。

組込みメソッドの呼出しキャッシュ・`array.append` のインライン化・整数 `==`/`!=` の高速化（内包表記ベンチ `tools/bench/comp.sp` で約25%短縮）、コンストラクタ高速パス（定数初期化のclassはinitを実行せず直接インスタンスを構築）、int除算/剰余のインライン化も加わり、クラス/メソッドのベンチは約30%、GC負荷ベンチは約40%短縮しました。

同梱moduleのsource hashを画像を書くときまで遅延し、キーワード判定と名前解決の `strlen` 呼出しを除いたため、`import gui` の起動は約15%速くなりました（モジュールが約30%大きくなった後でも）。ベンチ（`tools/bench/`）は2.2と同等かそれ以上で、`tools/bench/gui.sp`（GUIの描画・ソート・選択400回）を追加しています。

測り方は `tools/bench/`（`make bench`）。2つのバイナリの比較は `python3 tools/bench/cmp.py OLD NEW [RUNS]`（最小user CPU時間）。

### Copy-and-Patch JIT (2.9.0、2.10.0で関数呼出し、2.11.0でオブジェクトに対応)

x86-64 Linux では、熱くなった**ループ**を機械語にコンパイルします。方式は Copy-and-Patch です。あらかじめアセンブリで書いた小さな機械語片（ステンシル、`src/runtime/jit/stencils.S`）をバイトコードの順に`memcpy`し、スロットのオフセット・即値・分岐先の穴をパッチして繋ぐだけで、実行時にコード生成器を持ちません。

- 後方jumpが約40回実行されたループが対象です。変数の型（int / float / bool / array）をループ入口で一度だけ検査し、その仮定の下で型検査のない算術・比較・分岐・配列アクセス・`for x in array`・`array.append`・`len(array)`・`math.sqrt/sin/cos/exp/tan/atan` を並べます。型が分からない値（配列の要素）は使う場所で検査します。
- JITコードは本物のフレームと本物のoperand stackの上で動くため、ループを出る・仮定が外れる（オーバーフロー、0除算、範囲外index、型の不一致、ステップ予算切れ）ときは、**その命令の効果が起きる前に**インタプリタへ戻り、インタプリタが同じ命令をやり直します。意味論はインタプリタと一致します（`make test-jit` が全テストをしきい値1で、`tools/jitfuzz.py` が乱数プログラムで差分を確認します）。
- **2.10.0: 関数呼出し。** 約40回呼ばれた関数（引数の個数が固定、デフォルト値・可変長なし）は関数ごと機械語にコンパイルされ、コンパイル済み同士はネイティブの`call`で直接呼び合います（インタプリタと同じ`Frame`を積むので、エラートレース・コールスタック・step budgetは同一）。再帰、クロージャ（アップバリュー）、モジュール変数を書き換える関数も対象です。呼出し先が未コンパイル・組込み関数・引数の型が想定外・深さ上限などのときは、**呼出しの効果が起きる前に**ネイティブ呼出しの階層ごと巻き戻してインタプリタへ戻り、インタプリタが続きを実行します。
- **2.11.0: オブジェクト。** インスタンスのフィールド参照・代入（クラスをガードして固定スロットを直接読み書き）、メソッド呼出し（`obj.method(...)`は受け手のクラスをガードしてネイティブ呼出し）、`Class(...)`によるインスタンス生成（インタプリタの構築fast pathと同じ手順。割当てだけはCのヘルパを呼び、GCが走り得るのでスタックを整えてから呼びます）、`return`のコンストラクタフレーム対応が入りました。コンパイル済みコードが参照するクラスやメソッドはGCの根として保持されます。呼出し先が既知のときは、プロトタイプ検査だけで即値を使うネイティブ呼出し（自己再帰は直接`call`）に特化します。
- 例外・テーブルや文字列の操作・`print`などの組込み呼出しを含むコードはコンパイルされず、そのままインタプリタで動きます（遅くなりません）。`--no-jit` または環境変数 `SPRINT_JIT=0` で無効、`SPRINT_JIT_THRESHOLD=N` でしきい値、`SPRINT_JIT_DEBUG=1`（`2` で出口も）で経過をstderrに出します。SprintOS・非x86-64・`-DSP_NO_JIT` ではJITなしでビルドされます。

`tools/bench/cmp.sh` の結果（秒、最小の壁時間、同一マシン。`jit/lj` は Sprint(JIT) ÷ LuaJIT 2.1、小さいほど速い）:

| bench | Sprint JIT | Sprint (JITなし) | Lua 5.4 | LuaJIT | jit/lj |
|---|---|---|---|---|---|
| loop | 0.30 | 0.71 | 0.78 | 0.26 | 1.2 |
| collatz | 0.23 | 1.32 | 1.80 | 0.46 | 0.5 |
| sieve | 0.18 | 0.33 | 0.50 | 0.11 | 1.7 |
| nbody | 0.25 | 0.65 | 0.60 | 0.23 | 1.1 |
| arr | 0.06 | 0.14 | 0.18 | 0.03 | 1.8 |
| matmul | 0.04 | 0.19 | 0.19 | 0.02 | 2.3 |
| sort | 0.22 | 0.23 | 0.33 | 0.31 | 0.7 |
| str / strops | 0.13 / 0.06 | 0.13 / 0.06 | 0.48 / 0.15 | 0.31 / 0.09 | 0.4 / 0.7 |
| tblit | 0.12 | 0.12 | 0.78 | 0.09 | 1.5 |
| tbl | 0.25 | 0.21 | 0.39 | 0.11 | 2.2 |
| fib | 0.17 | 0.53 | 0.59 | 0.11 | 1.5 |
| closure | 0.10 | 0.37 | 0.46 | 0.02 | 6.6 |
| oop | 0.11 | 0.48 | 0.71 | 0.01 | 15 |

数値ループ（loop・collatz）はLuaJIT並みかそれ以上、nbody・sieve・arr・fibは2倍以内まで来ました。JITなしでもLua 5.4より速い（nbodyだけ僅かに遅い）です。**LuaJITに大差で負けるのは、LuaJITがインライン展開と割当ての除去でほぼ消してしまう処理（closure・oop）と行列積（matmul）**です。Sprintのコンパイラはループ・関数単位の領域JIT（型ガード+インタプリタへの安全な離脱）で、トレース記録・領域をまたぐレジスタ割当て・アロケーションの除去はまだ持ちません（2.14.0で小さな関数のインライン展開、2.17.0で式の途中の値とローカルのレジスタ保持が入りました）。次の課題はこの3つです。

- **2.11.1: 小さな最適化。** 書き込み先が整数型と確定している変数への整数演算ではタグの書き込みを省き、直前の演算結果がraxに残っている場合は次の演算・比較分岐の先頭のロードを省きます（結果は従来どおりメモリにも書くので、出口の挙動は変わりません）。fibなど呼出し中心のベンチで効果があり、パリティ分岐が支配的なcollatzではほぼ変わりません。

- **2.12.0: 局所変数の rbp 写し置き。** ループ領域・関数ユニットごとに、領域内で書き込まれ常に整数型の局所変数を最大1つ選び、その値を `rbp` にも保持します。メモリ側は常に最新のまま保つため、出口（インタプリタへ戻る処理）で書き戻す必要がなく、安全です。変数が最初のオペランドになる整数演算と比較分岐は、メモリを読まず `rbp` から取り、演算結果は `rbp` とメモリの両方へ書きます。ループ間で受け渡される値の依存連鎖がメモリ経由でなくなるため、sieve・collatz・closure が1〜2割速くなります。呼び出し先のユニットが `rbp` を使う場合は、そのユニットが入口で保存し戻りで復元します。`SPRINT_JIT_PIN=0` で無効化できます。

- **2.13.0: JITなしの高速化と浮動小数のレジスタ再利用。** インタプリタの頻出命令（量子化した融合命令と、LOAD_LOCAL・ADD・JUMPなどの基本命令）は、末尾でそれぞれ次の命令へ直接ジャンプする「スレッデッドディスパッチ」に変わりました（ラベルのアドレスを使うGNU拡張。ファイル内の `#pragma GCC diagnostic` で `-pedantic` の警告だけを局所的に抑えています）。1か所の `switch` に分岐が集まらなくなり、分岐予測が効いて、JITなしの collatz・loop・oop・sieve が1〜3割、arr が約3倍速くなりました。JIT側では、浮動小数演算の結果が `xmm0` に残っている場合に次の演算（`+` `*` は交換して）と `math.sqrt` の先頭ロードを省き、書き込み先がfloatと確定している演算ではタグ書き込みも省きます。nbody 0.35→0.26 秒。`tools/jitfuzz_float.py` で浮動小数式のランダム検証ができます。

- **2.14.0: 小さな関数のインライン展開。** 分岐・呼出し・アロケーション・モジュール変数アクセスを含まない短い関数（算術、`self.x` の読み書き、アップバリュー（クロージャ変数）の読み書き）は、呼出し側のコードの中へ展開します。呼出しの確認（関数のプロトタイプ・命令バジェット・フレーム数・スタック余裕）を1回行い、フレームを積まずに本体を実行します。展開した本体の途中でガードが外れると、その時点でインタプリタが積むはずのフレームを組み立ててから戻ります（`jit_finish`）。メソッド呼出し（`obj.m(...)`）も、受け手のクラスをガードした上で同様に展開します。`SPRINT_JIT_INLINE=0` で無効化できます。closureは約0.14→0.10秒。ランダム検証は `tools/jitfuzz_inline.py`（型・オーバーフローを途中で変えて、展開内の出口を意図的に起こします）。
- **2.15.0: メモリ削減。** `Array.sort` を作り直しました。既定の比較で全要素が整数のときは一時的な int64 キー配列でその場ソートし、それ以外は2本のバッファを往復するマージソートで、比較関数を使うときだけスナップショットを確保し、一時領域は使い終わり次第すぐ解放します。また、CLIでは glibc の `M_MMAP_THRESHOLD` を 256KB に固定し、大きな配列・文字列を解放と同時にOSへ返すようにしました（sieve のピークRSSは約133MB→81MB、Lua 5.4 は約134MB）。sort.sp のピークは約27MBで、Lua（約13MB）にはまだ届いていません。
- **2.16.0: 型が予測できるフィールド読み出しの融合。** `self.x` のようなフィールド読み出しで、フィールドの型（整数か浮動小数点）がコンパイル時に予測できる場合、型の確認・値の読み出し・レジスタへの保持を1つのステンシル（`ldfld_i`/`ldfld_f`）にまとめました。後続の算術が直前の値を再読み込みせず、別個の型ガードも不要になります。予測が外れた場合は通常の出口でインタプリタに戻ります。oopは約0.132→0.121秒、nbodyは約0.312→0.293秒。
- **2.17.0: レジスタ割り当て。** 型の分かっている純粋な値（フィールド、配列の要素、`+ - * /`、`math.sqrt` の結果）を、スタックスロットではなくマシンレジスタ（整数・ポインタ6本、`xmm` 7本）に置くようにしました。値は「保留中」の項目なので、ガードが外れたときは従来どおり再実行で戻れます（出口はレジスタを知りません）。代入したローカルはそのレジスタをミラーとして持ち続け（メモリが正本のwrite-through）、同じ基本ブロック内の読み出しはメモリに触れません。比較分岐・`SET_MEMBER`・`SET_INDEX` もレジスタの値を直接使います。また `range` ループの変数が rbp のミラーを持つ場合の余分なコピーを減らしました。matmulは約0.052→0.041秒、nbodyは約0.30→0.25秒、oopは約0.12→0.11秒。`SPRINT_JIT_REGS=0` で無効化できます。ランダム検証は `tools/jitfuzz_reg.py`。
- **2.18.0: インスタンス生成のインライン化とupvalueのレジスタ化。** `new`/コンストラクタ呼び出しのプール割り当て（サイズクラスのfree list）をスタブ内に展開し、ランタイム呼び出しを省きました。クロージャの upvalue 読み書きと、インライン展開した関数の `RETURN` もレジスタの値を直接扱います。oopは約0.113→0.098秒、closureは約0.111→0.096秒。インライン内でのローカル代入が呼び出し側の保留レジスタを誤って書き戻す不具合（`tools/jitfuzz_inline.py` で発見）も修正しました。x86-64のみ。
- **2.19.0: コンストラクタのインライン展開。** `Class(args)` の `init` が直線コード（フィールド代入・算術・upvalueのみ）なら、本体を呼び出し側に展開します。引数は保留中の値のまま新しいインスタンスの上のスロットへ直接書き、インスタンスの割り当ては「スロット数と既定値をコンパイル時に埋め込んだ」ヘッダ初期化＋展開済みの既定値ストアになりました（ランタイム側のループなし）。`init` の途中でガードが外れたときは、インタプリタ側に構築フレーム（`FK_CTOR`）を作って続きから再開します。展開できなかった場合は呼び出しと引数を元の位置に戻して従来経路へ戻ります。oopは約0.101→0.082秒。ゲートは `tests/cases/jit/jit_ctor.sp`。

**メモリ（2.11.0）**: オブジェクトヘッダを24→16バイトにし、GCの最小しきい値を4MB→1MBにしました（速度は変わらず）。起動直後は約2.3MB（Lua 5.4は約2.6MB）、oop 6.9→3.5MB、tbl 7.4→3.5MB。連結（`..`）が毎回4KBのスクラッチ配列をゼロ初期化していた無駄も除きました（tbl 0.36→0.21秒）。

### Lua 5.4 との比較 (2.8.0)

`tools/bench/cmp.sh`（Lua 5.4 版は `tools/bench/lua/`）で同じ処理を並べて測ります（最小の壁時間、同一マシン、`lua5.4`）。2.8.0 の結果は下の通りで、ほとんどがLuaと同等以上（比 < 1 がSprintの方が速い）です。

実プログラム風のベンチ（2.31.0）: `tokenize.sp`（字句解析。述語関数・文字列の切り出し・表への追加）、`minivm.sp`（小さなバイトコードVM）、`wordfreq.sp`（`split`・文字列キーの表の更新・コールバック付き `sort`）、`tsieve.sp` / `tmatmul.sp`（`typed` 配列）。`cmp.sh` は `tools/bench/<名前>.sp` と `tools/bench/lua/<名前>.lua` が両方ある組を自動で並べます（`wordfreq.lua` は `//` を使うので LuaJIT では動かず、LuaJIT の欄は 0 になります）。

| bench | 比 | | bench | 比 |
|---|---|---|---|---|
| arr | 0.7 | | oop | 0.65 |
| closure | 0.7 | | sieve | 0.6 |
| collatz | 0.8 | | sort | 0.7 |
| fib | 0.75 | | str / strops | 0.35 / 0.5 |
| loop | 0.85 | | strcat | 0.9 |
| matmul | 0.95 | | tbl / tblit | 0.9 / 0.2 |
| nbody (float + `math.sqrt` 1000万回) | 1.05 | | | |

2.8.0 で追加: `LOAD_LOCAL k; <算術>`（`top op local`）・`x % m == k` を条件にした分岐・ループ末尾の `JUMP` から比較分岐への直行を融合命令にし、ディスパッチの範囲チェックを消し、テーブル/モジュールのメンバ参照にキャッシュを入れました、`math.sqrt/sin/cos/exp/tan/atan` を float 引数で呼ぶときの近道（collatz 1.95→1.5秒、nbody 0.9→0.58秒）。

主な変更: 長い文字列のハッシュを初回使用時まで遅延（`s = s .. "x"` の繰り返しが 31秒→2秒、二次的に遅かった致命的な点）、`s += x % k` / `s += x` / `a[i] = v` / `a[i][j]` / `x % k == c` / float演算・比較 / クロージャの `n += 1` などの融合命令、インタプリタ本体 (`vm.c`) のみ `-fno-strict-overflow` と関数単位のスタックcanaryを外す（他の全ファイルのハードニングは維持。理由は Makefile のコメント）。測定は共有VM上で±15%ほどぶれます。

## 状況

仕様書 §0.6 に実装状況の表があります。未対応: カスタムアロケータ、`lint --alloc`、`Sprint.toml` の `[dependencies]`。
- **2.20.0: for-range の符号別ステンシル。** `range(a, b, 定数)` のステップが即値の整数定数（0以外、32ビットに収まる）のとき、`RANGE_NEXT` はステップの符号判定とメモリからの再ロードをやめ、正方向用・負方向用の専用ステンシル（`range_up` / `range_dn`、ステップは即値穴 H2）に切り替わります。ステップが定数と分からない場合（変数、領域がループ先頭から始まる場合、インライン本体）は従来の `range_next` のままです。オーバーフロー時に stop へ丸める意味は同じです。ループ単体で約3.4倍（30M反復で0.37→0.11秒、JITなし比）。
- **2.21.0: インライン本体内でのローカルのレジスタ写し。** これまで `cur_ic == 0` に限っていた `lc_*` ミラーを、インライン展開した本体（メソッド・関数・コンストラクタ）でも使います。ミラーのキーは呼び出しごとに別の範囲（`1000 * cur_ic + 局所番号`）を持ち、本体の `RETURN` で解放します。本体のローカルの読み取り回数は `inl_body` が数えます。意味は変わりません。**実測では速度はほぼ変わりませんでした**（oop 80→78ms、closure 93→95ms、誤差の範囲）。律速は `add`（割り当てを含むためインライン不可）の通常呼び出しと、結果・引数のスロット往復です。
- **2.22.0: 入れ子のインライン展開。** インライン展開したメソッド・関数の本体の中の `Class(args)`（クラスはモジュール変数、`LOAD_MODULE` + `CALL`）も、コンストラクタを展開します（1段まで。展開できないときは本体ごと展開を諦めて通常呼び出しに戻ります）。`JIctx` に親へのリンク（`parent`）を持たせ、出口の復帰（`jit_finish`）は外側から順に複数のフレーム（メソッド用と `FK_CTOR`）を作ります。フレーム上限は仮想フレーム2枚ぶんの余裕を残すよう `max_frames - 1` にしました。oop は約73→61ms（最小値）。新ファザー `tools/jitfuzz_ctor.py`（コンストラクタ展開と入れ子展開、型の入れ替え・オーバーフロー・エラー注入を含む）と、ゲート `tests/cases/jit/jit_nested_ctor.sp` を追加しました。
- **2.23.0: 解放パスの高速化とインライン呼び出しの資源検査の巻き上げ。** (1) GC のスイープで、外部記憶を持たないインスタンスのブロックを `free_obj` / `obj_release_block` を経由せずサイズクラスのプールへ直接戻します。(2) ループ領域のインライン呼び出しが毎回行っていた「フレームの余裕」と「スタックの余裕」の検査をやめ、領域に入るときに一度だけ検査します（`JRegion.inl_extent`、`jit_site`）。足りなければ領域に入らずインタプリタが実行するので、深さ上限のエラーは従来どおり同じ場所で出ます。関数ユニット（再帰しうる）は従来の検査のままです。新しい雛形は `ginln` / `ginlrn`（命令数の予算のみ）。(3) インライン展開したコンストラクタの `init` の戻り値は捨てられるので、その書き出しを省きました。oop は callgrind で 1 反復あたり約312→約260命令、時間は約62→53ms（最小値）。ゲートは `tests/cases/jit/jit_inline_depth.sp`（深さ上限ぎりぎりの再帰）。
- **2.24.0: クロージャ変数・range の刻み・インスタンスを持つモジュール変数。** (1) upvalue のレジスタ写し: 読み書きした upvalue の値とセルのアドレスをレジスタに保持し、同じ変数の次の読み出し（型ガードとセルへのポインタ連鎖）と書き込み（連鎖）を省きます（`n += 1; return n`）。どの upvalue への書き込みでも値の写しは捨て（二つのクロージャが同じセルを共有しうる）、インライン本体への出入りでは全て捨てます。(2) range の刻み: 領域がループの途中から始まる場合（`RANGE_PREP` が領域の外）、領域に入るときの刻みを即値としてコンパイルし、入口で `gimm` により値を検査します（違えばインタプリタへ）。(3) インスタンスを持つモジュール変数に型（`TY_INST` とクラス）を持たせ、領域の入口で `gcls_mod` により検査します。未初期化検査とメンバーアクセスごとのクラス検査が消えます。呼び出しの後は従来どおり型を捨てます。oop は 1 反復あたり約260→約232命令、closure は約57→約47命令（時間は約74→59ms）。ゲートは `jit_range_entry.sp`、`jit_modinst.sp`。
- **2.25.0: 文字列キーの表と文字列連結の JIT 化。** `t[k]` の読み書き（表・文字列）と `a .. b .. c` が JIT ループの中で止まらずにコンパイルされます（Cヘルパー呼び出し。扱えない形はインタプリタへ戻って同じ命令を実行します）。tbl ベンチは約2613M→2203M 命令、時間は約190→約170ms です。残りの時間は整数の文字列化・割り当て・ハッシュ・探索・GCという実行時の仕事で、ディスパッチではありません。ゲートは `jit_tblstr.sp`。
- **2.25.1: 文字列まわりの実行時コスト削減。** 整数の文字列化（2桁テーブル）と連結（桁数を先に数えて結果へ直接書く）を軽くしました。tbl は 2203M→約2030M 命令ですが、実時間は新旧交互の測定で差がなく（約165〜170ms）、速度改善とは言えません。8バイト単位のハッシュは命令数が減るのに約6%遅く（store-forwarding の待ち）、採用しませんでした。strcat の約950msは 1文字追記の二乗コピー（200k回で約20GB、メモリ帯域）が原因で、C側の最適化では縮みません。ゲートは `int_concat.sp`。
- **2.26.0: 数学関数と `len` の JIT 化。** `math.abs / min / max / clamp / floor / ceil / round / log / atan2 …` と、文字列・テーブルに対する `len` が、JIT ループの中で止まらずにコンパイルされます（C ヘルパー呼び出し。エラーや型の予測違いはインタプリタで再実行）。natcall ベンチは約413→約163ms。ゲートは `jit_natives.sp`。
- **2.27.0: テーブルの for-in の JIT 化。** `for k, v in table` / `for k in table` が JIT ループの中でコンパイルされます（C ヘルパー。反復中の変更などはインタプリタがエラーにします）。tblit ベンチは約121→約80ms。ゲートは `jit_tabfor.sp`。
- **2.28.0: モジュール変数のレジスタ入出力。** トップレベルのスクリプトで `x = x + d` のようにモジュール変数を回すループが、スタック経由の16バイトコピーをやめてレジスタで動きます（手書きの数値ループ 24→14ms、命令数 322M→193M。ローカル変数版 12ms にほぼ並ぶ）。インスタンスを持つ変数のロードも同様ですが、oop ベンチは割り当て（インスタンスの生成と GC）が支配的で、ほぼ変わりません。ゲートは `jit_modreg.sp`。
- **2.28.1: JIT の誤コンパイル修正（インライン化されたコンストラクタ）。** `a + P(x).v` のように、呼び出し元が `P(...)` の下に未確定の値（ローカル・定数・フィールド読み出し）を積んだままコンストラクタをインライン化し、その中のガードが（エラーなしで）外れると、インタプリタが古いスタックスロットを読んで誤った結果になることがありました。`do_construct` が、クラスの下にある項目を先にメモリへ書き戻すよう修正しました。他のインライン入口（`do_call`、`do_invoke_inst`、入れ子の呼び出し/コンストラクタ、ヘルパ呼び出しのステンシル）は監査して安全を確認済みです。ファザー `jitfuzz_ctor.py` に未確定オペランドの生成を加え、修正前のビルドで検出できることを確認しました。ゲートは `jit_pending_ctor.sp`。
- **2.29.0: 型付き配列 `typed`。** `import typed` で `typed.f64(n)` `typed.i32(n)` `typed.u8(n)`（ほかに `i8 u16 u32 i64 f32`）が使えます。要素を箱詰めせず1〜8バイトで持つ長さ固定の配列で、`a[i]` `len(a)` `for x in a` `fill` `copy` `slice` `to_array` `set_from` を備えます。整数は下位ビットへ切り詰め（`u8` に300→44）、`f32` は単精度へ丸めます。ホットループでは JIT が型タグ・クラスのガードと範囲検査のあと要素へ直接アクセスする機械語を出すので、ふつうの array より速く小さくなります（5,000,000 までの素数ふるい: array 0.236s / 78MB → `typed.u8` 0.055s / 12MB）。JIT を使わない場合もインタプリタに専用の高速パスがあります。ゲートは `jit_typed.sp` `typed.sp`、ファザーは `tools/jitfuzz_typed.py`（意図的に壊した JIT でミスマッチを検出することを確認済み）。仕様は §15.20。
- **2.30.0: 型付き配列のループとインライン化。** `for x in a` / `for i, x in a` が `typed` の配列でも使え、JIT もループを翻訳します（配列の添字ではなく `for` で回すコードが速くなる）。また、メソッドや小さな関数の本体の中の添字（`self.d[i]`、`arr[i] = v`）をインライン化できるようになり、通常の array でも typed でも、メソッド呼び出しごとの呼び出しコストが消えます（添字を呼ぶメソッドを回す手書きループで約2割短縮）。ファザー `tools/jitfuzz_inlidx.py` を追加しました。
- **2.31.0: 比較・`and` / `or` を「値」として使う関数の JIT 化。** `func is_digit(c): return c >= 48 and c <= 57` のように、比較の結果や `and` / `or` の結果を返す・変数に入れる・引数に渡すコードが、これまで「comparison used as a value」「and / or used as a value」でコンパイルされず、インタプリタで走っていました。今回、整数（6種）・浮動小数点（`< <= > >=`）の比較をレジスタ上で行って `setcc` で真偽値を作り、`not (a < b)` は条件を反転して同じ形で処理します（NaN でも正しい）。`and` / `or` は、評価した値をメモリのスタックに残して分岐し、合流点では「スタックの深さと、値がメモリにあること」を検査してから合流します（`ldep`）。さらに `x ?? y` / `x == nil` / `x != nil` の分岐（`DUP; NIL; NE; JUMP_IF_TRUE` の形）を、タグ比較1命令の新ステンシル `nilbr_t` / `nilbr_f` で翻訳します。実プログラム風の新ベンチ `tools/bench/tokenize.sp`（字句解析）は約0.84秒→約0.21秒（約4倍）になりました（Lua 5.4: 約1.05秒、LuaJIT: 約0.07秒なので、LuaJIT にはまだ約3倍の差があります）。`wordfreq.sp`（`split` と文字列キーの表）は約0.28秒で、Lua 5.4 の約0.25秒とほぼ同じです。残る時間は文字列の割り当て・ハッシュ・探索という実行時の仕事です（`??` の JIT 化では変わりませんでした）。ゲートは `jit_boolval.sp` と `jit_nilbr.sp`、ファザーは `tools/jitfuzz_bool.py`（ミューテーションで、cc 表・反転・`or_pop` の分岐の向き・nil 分岐の向きの取り違えをすべて検出することを確認済み）です。配列リテラル（`[a, b]`）を含む関数・ループは従来どおり JIT 化されません。

協力:Claude(I love Claude)
