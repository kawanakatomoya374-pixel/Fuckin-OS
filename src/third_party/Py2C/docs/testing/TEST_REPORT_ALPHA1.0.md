# Python Code to C Alpha1.0 — 検証証跡

この文書は、客観監査改修後に実行した検証の結果を記録します。対象はHosted CLI、ホストGUI、freestanding静的ライブラリ、生成C、GC、GUIコマンドバッファ、構文・意味論回帰です。

| 検証 | 実行内容 | 結果 |
|---|---|---|
| GCCフルビルド | `make CC=gcc full-build`、CLI・GUI・freestanding | 拡張警告を含む警告即エラーで合格 |
| Clangフルビルド | `make CC=clang full-build`、CLI・GUI・freestanding | 拡張警告を含む警告即エラーで合格 |
| 通常ビルド | `make -j2`、`-std=c11 -Wall -Wextra -Werror` | 合格 |
| GCC全回帰 | `make CC=gcc test` | 合格 |
| Clang全回帰 | `make CC=clang test` | 合格 |
| コンフォーマンスコーパス | 横断コーパス、dict/set内包表記、複数with、辞書順序・等値比較、固定幅整数、評価順、型注釈、代入式、コンテナ拡張メソッド、match/case guard、辞書マージ、bare raise、文字列拡張、for starred unpack、`print(sep=..., end=...)`、`str.index`/`rindex`、回数指定`replace`、範囲指定`list.index`、文字列探索範囲、`split(maxsplit)`、`strip(chars)`、prefix/suffix範囲、partition、文字種判定、splitlines、expandtabs、制御文字リテラル/表示、`dict.fromkeys`、pair iterable `update`、set包含関係・`pop`、generator再開・for反復、`async def`・await・`asyncio.run`、Python 3.13位置専用引数、`yield from`、`raise from`、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum` | **304件のCPython差分一致** |
| 意味論監査 | short-circuit、loop-else、単一・複数with、複合代入、`del`、bit演算、set/dict内包表記、ネスト関数診断 | 合格 |
| GC/GUI C回帰 | `make test-gc-gui` | `gc_gui_ok`。GCルート重複登録、GUIコマンド上限、テキストアリーナ完全使用・超過ドロップを検証 |
| GUIビルド | `make gui` | 合格 |
| freestanding | `make freestanding`、`make CC=clang -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=.` | 汎用C11テンプレートから`libpython-code-to-c-core.a`生成成功。platform中核にHosted libc未解決参照なし |
| 単一ヘッダー | `make test-single-header`、`make CC=clang test-single-header-c11`、`make CC=clang test-single-header-freestanding` | 外部Py2cソースをリンクしない自己完結ビルドが成功。Python 3.13型構文、位置専用引数、`yield from`、`raise from`、walrus、match/case、辞書マージ、for starred unpack、`print`キーワード、文字列探索範囲、`split(maxsplit)`、`str.index`/`rindex`、回数指定`replace`、範囲指定`list.index`を検査。Clang単独経路は`-pedantic-errors`を使用し、freestanding経路はHosted allocator・出力・時刻参照の不在を検査。数値変換・書式化・数学補助はOS提供契約 |
| ISO C11受理 | `--c11 tests/c11_safe.py`後に`-std=c11 -pedantic-errors -Wall -Wextra -Werror` | 合格 |
| ISO C11拒否 | `--c11 tests/complex_alpha10.py` | GNU拡張を要する構文を明示診断 |
| ネスト関数診断 | `tests/nonlocal_audit_alpha10.py` | 移植可能なクロージャ未実装を変換時に明示診断 |
| 64ビット整数境界 | `make test-integer-overflow` | 加減乗・除算・累乗・シフトが未定義動作にならないことを直接C APIで確認 |
| 決定的コンテナファジング | `make test-container-fuzz`、追加8 seed × 64操作 | 既定3 seed × 48操作（144操作）と拡張512操作のCPython差分一致。失敗時は最小prefixを保存 |
| Clang静的解析 | common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュール | 警告0件 |
| GC容量設定 | `-DP2C_GC_ROOT_CAPACITY=8`でGC/GUIテストをビルド・実行 | 合格 |
| プラットフォーム出力 | `make test-platform-adapter` | 登録済み`P2C_Platform.write`へ`print(sep=..., end=...)`の出力が完全に到達 |
| Python 3.13型構文 | `make test-py313-syntax` | 型パラメータ既定値、TypeVarTuple、ParamSpec、soft keyword `type`文を型消去して警告即エラーの厳格C11生成Cで実行 |
| ジェネレータ・協調async | `make test-generator-async-runtime`、`make test-async-generator` | 状態保存、停止・再開、StopIteration、`yield from`値委譲、ネストawait、FIFO協調実行、GC到達性、CPython差分を確認 |
| ベアメタル実行 | `make test-baremetal-runtime` | `PYTHON_CODE_TO_C_NO_STDLIB`、静的ヒープ、platform write、tick、GC、協調awaitを警告即エラーで実行 |
| ベアメタル生成C | `make test-baremetal-build`、`make test-baremetal-generated` | 起動・アダプタ例をfreestandingアーカイブ化し、`baremetal_hello.py`（ネストしたクラス定義を含む）を実変換してC11オブジェクト化 |
| ネストしたクラス定義 | CPython差分 C461–C479、`make test-decorator-diagnostics`、`make test-embed-generated`、`make test-baremetal-generated` | 19件一致。`Outer.Inner`経由の構築・メソッド呼出し、クラス本体での名前参照と上から下への評価順、同名ネストクラスの衝突回避（`Left__Node`/`Right__Node`）、二段ネスト、`isinstance(x, Outer.Inner)`を確認。関数本体内の`class`定義とメソッド本体からのクラススコープ参照は明示診断。組込み例（`embed_boot.py`/`baremetal_hello.py`）でも変換・実行 |
| C識別子の衝突回避 | CPython差分 C480–C486、`make test-baremetal-generated` | Cライブラリ名（`index`/`round`/`abs`/`pow`/`sqrt`/`log`/`main`）をパラメータ・ローカル・モジュール関数名に使った7件が一致。前方宣言・パラメータ宣言・`(void)`キャスト・本体参照のマングル名統一を確認 |
| 文字コード・基数変換 | `tests/numeric_text_builtins_alpha10.py`、C288–C299、単一ヘッダー | UTF-8単一Unicodeスカラー値の`ord`/`chr`、負値を含む`bin`/`oct`/`hex`、開始値付き`sum`がCPythonと一致 |
| 名称監査 | Alpha0.3および旧製品名の残存走査 | Alpha1.0正本で残存なし |

## Sanitizer検査

ASan/UBSan付きでコンパイラ本体と複雑コーパスを実行しました。未定義動作・不正アクセスを示す障害はこの検査で再現しませんでした。監査中にAST、内包表記、先読みトークン、意味解析シンボル、コード生成マップの主要なリークを特定・修正しました。

LeakSanitizerで過去に確認された短命なCLIコンパイラ側の部分解析経路および中間構造の所有権課題は、生成プログラムのGCリークとは別に継続管理する。今回のAlpha1.0品質ゲートでは、common、platform中核、Hostedアダプタ、runtime、parser、AST、AST dump、lexer、codegen、semanticの10モジュールに対するClang静的解析を診断0件で通過させた。最新の304件コーパス、GCC/Clang双方のHosted・GUI・freestandingビルド、Python 3.13型構文・位置専用引数・`yield from`・`raise from`、UTF-8文字コード・基数変換・開始値付き集計の回帰、状態機械ジェネレータ・協調async回帰、静的ヒープのベアメタル実行、実変換ベアメタルCのC11オブジェクト化、高次dict/set拡張を含む単一ヘッダー自己完結ビルド、Clang単独の厳格C11単一ヘッダー経路、Hosted allocator・出力・時刻参照を除外するfreestanding単一ヘッダー経路、プラットフォーム出力アダプタ、`make run`および汎用C11テンプレートの起動契約、決定的コンテナファジング、およびネスト関数の移植性診断は、`docs/STRICT_REVIEW_ALPHA1.0.md`に記録した。

> この結果は、残課題を隠した「完全無欠」の宣言ではありません。対応済み範囲を厳格に検証し、未達範囲は変換診断・文書・次期優先順位として明示する品質方針に基づく記録です。

## make help の全ターゲット実行（Alpha1.0）

`make help` に列挙される全ターゲットを順に実行するランナー `tests/run_all_make_targets.sh` を
追加し、全件を実行・確認しました（結果は `/tmp/helprun/summary.txt` に記録）。

| 区分 | 結果 |
| --- | --- |
| ビルド | all / gui / single-header / freestanding / c99 / tcc すべて成功 |
| 差分・回帰 | test（661 s・0エラー）/ test-c99（770件一致）/ test-tcc（770件一致）/ test-fallback / test-sandbox ほか |
| 組込み | test-embed-runtime / test-embed-compile / test-embed-generated / test-baremetal-* / test-gc-* / test-stack-usage |
| 解析 | test-analyzer（263 s・欠陥0）/ test-analyzer-clang（106 s・欠陥0）/ test-sanitizers / test-sanitizers-core |
| 動的解析（厳格） | test-asan-strict / test-ubsan-deep / test-msan / test-clang-integer / test-harden-generated / test-hardened-core / test-avinit-differential / test-lsan-generated （`tests/strict_batch_alpha10.sh` で一括。詳細は docs/testing/STRICT_BUILD_AND_ANALYSIS_ALPHA1.0.md） |
| ELF | elf / hobbyos-elf / tcc elf c99 すべて成功 |

### 実行で見つかった不具合と修正

| 症状 | 原因 | 修正 |
| --- | --- | --- |
| `make hobbyos elf` が "No rule to make target 'hobbyos'" で失敗 | `help` が案内する `hobbyos` ゴールが未定義（`hobby` のみ定義されていた） | `hobbyos:` を別名ターゲットとして追加し `.PHONY` に登録（HobbyOS ELF の選択は従来どおり `elf` 側がゴールから判定） |
| `make tcc elf hobby c99` が "ROOT: parameter not set" で失敗 | `tests/build_tcc_elf_alpha10.sh` が `$(MAKE) hobbyos-elf` を実行しており、Make変数が展開されず `MAKE` をコマンドとして起動していた | `${MAKE:-make}` と `$(dirname "$0")/..` で自己完結化し、TinyCC の `-nostdlib` リンク失敗時に GCC リンカスクリプト経路へ正しくフォールバック |

修正後の実測: `make hobbyos elf` → `hobbyos_elf_ok: build/hobbyos/python-code-to-c-hobbyos.elf`、
`make tcc elf hobby c99` → `hobbyos_elf_ok: build/hobbyos-tcc/python-code-to-c-hobbyos.elf`（どちらも rc=0）。
`make run INPUT=...` と `make freestanding` も実引数で成功を確認しています。

### 追加修正（クラス名の隠蔽順序）

フィクスチャを変換・ビルドして CPython と差分比較する補助 `tests/quick_diff_alpha10.sh` を追加し、
実践的な複合プログラム（`tests/complex_program_alpha10.py`）を回帰へ加えたところ、
**3段継承で `super().__init__(a, b)` が基底の `__init__` を呼ばず、祖先のメソッドが
`AttributeError` になる**不具合を検出しました。

- 原因: クラス名レジストリの名前引き（`p2c_find_class_by_name` / `p2c_find_class_by_span`）が
  **先に登録された**クラスを返していました。起動時にモジュール側のクラス
  （pygame の `Rect`/`Surface`/`Sprite`/`Group`/`Clock`）が登録されるため、ユーザーの
  `class Rect(Shape)` ではなく pygame の `Rect`（基底なし・無言の `__init__`）が継承元として
  解決され、`p2c_class_mro(Square)` が `[Rect]` で打ち切られ、`Shape` 由来の `describe` が
  未解決になっていました（`super()` の探索も同名の別クラスへ向かっていました）。
- 修正: 名前引きを**後から登録された定義優先**（逆順探索）に変更。ユーザー定義クラスが
  ランタイム／モジュール側の同名クラスを隠します（Python の「後から定義した名前が勝つ」と同義）。
- 回帰: `C577–C588`（複合プログラム）と `C589–C593`（隠蔽規則と3段継承の `super()`）を追加し、
  合計 **770** アサーションになりました。
