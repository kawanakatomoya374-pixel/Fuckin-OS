# バグ修正レポート（Alpha1.0 追加修正）

本ドキュメントは、Alpha1.0 のレビューで検出した不具合の修正内容を記録する。
いずれも `tests/fixes_alpha10.py`（`tests/conformance_regression.sh` の F01–F33）
と既存の回帰で保護している。

## 1. freestanding / NO_STDLIB 構成のビルド不能（最重要）

- 症状: `make freestanding`、`make test`（263行目で停止）、`test-heap-unification`、
  `test-stack-usage`、`test-hobby-os-template`、`test-baremetal-*`、`test-embed-*`、
  `test-single-header-freestanding` が `UINT64_MAX`/`INT64_MAX` undeclared で失敗。
- 原因: `include/common/python_code_to_c_common.h` の自前型定義分岐
  （コンパイラ提供 `<stdint.h>` を使わない構成）が 64bit リミットマクロを
  定義しておらず、`parser.c` / `runtime.c` が範囲検査で参照していた。
- 修正: 同分岐に `INT64_MAX` / `INT64_MIN` / `UINT64_MAX` を追加。
  `tools/generate_single_header.sh` を再生成。
- 効果: 上記ターゲットが全復旧（`make test` が最後まで実行可能に）。

## 2. 不正な C を出力していたコード生成

生成コードは「未対応は位置付き診断、不正なCは出さない」契約だが、以下は
コンパイルエラーになるCを出力していたため修正した。

| 症状 | 原因 | 修正 |
|---|---|---|
| `double = 2` / `struct = [1]` 等で `expected identifier` | `mangle_ident()` が C11予約語を回避していなかった | C11キーワード（`double`/`int`/`struct` 等）を `p2c_user_` 前置へ追加 |
| `a = b = 1` の後にもう一度連鎖代入 → `_p2c_assign_tmp` 再定義 | 一時変数名が固定 | 連鎖代入の一時変数を `cg->temp_counter` で一意化 |
| `g = f` / `sorted(key=f)` / `map(f, ...)`（通常のdef） | 関数名を値として参照する経路が無く、生のC関数ポインタを出力 | module関数の callable adapter を生成し `p2c_function_new` で包む（値参照された関数のみ生成し未使用警告を回避） |
| `def f(a, b=a if a else 0)` | 既定値式を呼び出し地点へそのまま展開 | （既定値の定義時評価は未対応のため）参照エラーになる生成を避ける診断と既定値補完を整理 |
| クロージャ内のさらに入れ子の `def`（デコレータファクトリ等） | 静的関数を関数本体へ入れ子出力 | ネスト定義を別バッファへ退避し forward 末尾へ連結（ラムダと同じ方式）。`*args/**kwargs` も束縛を生成 |
| `issubclass(A, B)` | 未対応ビルトインを生の関数呼び出しとして出力 | `p2c_issubclass_of_class/object` を追加して解決 |
| 捕捉した callable の呼び出し `fn(x)`（高階関数・デコレータ内） | 呼び出し経路が closure/cell を解決せず `fn(...)` を出力 | `p2c_call(p2c_cell_get(...), ...)` へ解決（`*args/**kwargs` 付きは明示診断） |

## 3. 誤った値を静かに返していた意味論

| 症状 | 原因 | 修正 |
|---|---|---|
| `(1,2) + (3,)` が `0` | `p2c_obj_add` に tuple+tuple が無く整数加算へ落下 | tuple 連結を実装 |
| `b += [3]` / `s \|= {3}` / `a *= 2` が別名に反映されない | 複合代入が新オブジェクト生成＋再束縛 | `p2c_obj_iadd/isub/imul/ibitand/ibitor/ibitxor` を追加し in-place 化 |
| `bool(obj)` が常に True | `p2c_obj_is_truthy` が `OBJ_INSTANCE` を default で true | `__bool__`（無ければ `__len__`）を参照 |
| `isinstance(True, int)` が False | bool を int のサブクラスとして扱っていなかった | `p2c_obj_is_int_like` を追加し isinstance(int) で使用 |
| `round(1234.5678, -2)` が 1235.0 | 負の桁数を 0 扱い | 10^-n 単位の最近偶数丸め（int/float を保持） |
| `min([])` が `[]` を返す | 空 iterable を「非iterableの単一値」と誤認 | 件数を採用し `ValueError` |
| `repr("it's")` が `'it\'s'` | 常にシングルクォート | ダブルクォートが有利なら切り替え |
| `print(range(3))` が `<object>` | print 経路に `OBJ_RANGE` が無い | range 表記を追加 |
| `r.start` が AttributeError | range 属性（start/stop/step）未対応 | `getattr_raw` に追加 |
| ユーザー例外の `str(e)` が `<MyError object>` | コンストラクタ引数を保持していなかった | 直系 Exception 派生で `self.args` を保存し `Exception.__str__` 相当を実装 |
| `"{:%}".format(0.25)` が `%g` | `str.format` が `%` 指定を解釈していなかった | 100倍＋既定6桁＋`%` のパーセント書式を実装 |
| `*x, y = (1,2,3)` の `x` が tuple | スライス結果をそのまま代入 | list へ変換（Python の star target は list） |

## 4. パーサ

- `lambda x, y=10: ...` が構文エラーだったため、ラムダの既定引数を受理。

## 5. 回帰テストの更新

- `tests/test_embed_transpile.c`: `+=` が in-place 版 `p2c_obj_iadd` を使うよう
  変更されたため、生成コード検査を追随。
- 新規 `tests/fixes_alpha10.py`（21アサーション）を `tests/conformance_regression.sh`
  へ F01–F21 として登録。

## 6. 既知の未対応（本修正の対象外）
- ループ本体の `yield`（ジェネレータ関数の状態機械）。
- `*args/**kwargs` を持つデコレータ／可変長引数付き callable の呼び出し
  （明示診断。`@decorator` 側の可変長は非対応）。
- `str.format` のネスト置換フィールド（`"{:>{}}"`）と添字/キー参照（`"{0[0]}"`）。
- 既定値を定義時に一度だけ評価する意味論（可変既定値の共有）。
- `type(None)` を型として使う `isinstance(x, type(None))`。
## 7. 追加修正（堅牢性・意味論の徹底）

本方針で追加検出した不具合。CPython 差分と厳格プロファイル（clang /
サニタイザ / `-fanalyzer`）で確認している。

### 7.1 不正なC / クラッシュ / 例外漏れ

| 症状 | 原因 | 修正 |
|---|---|---|
| `raise X from ValueError("m")` や `e = ValueError("v")` が未定義関数呼び出し | 式位置の組み込み例外構築が未実装 | `p2c_make_exception` へ解決 |
| `hash(x)` が不正なC | ビルトイン未対応 | `p2c_builtin_hash` を追加 |
| `len(1)` / `len(None)` が `0` を返す | `p2c_len` の default が 0 | 非サイズ型は `TypeError` |
| `a **= 2` が `a += 2` になっていた | 複合代入の `OP_POW` 未マップ | `p2c_obj_pow` へマップ＋未知演算子は診断 |
| `def __init__(self, *args, **kw)` が args 再宣言でビルド不可 | 生成フレーム名と衝突 | 生成内部名（args/nargs/env/generator 等）をマングル |
| クロージャ内のさらに入れ子の def が関数内 static 定義で不正C | 定義の割り込み | 別バッファへ退避し forward 末尾へ連結 |
| デコレータファクトリの多段 capture が未定義名 | 捕捉が1段のみ | 外側 closure env のセルを共有（多段 capture 対応） |
| `f(*a, **k)` が引数不足の不正C | 混在展開がベストエフォート | 組み立て不能な場合は明示診断 |
| 未定義名の裸参照が不正C | 生のC識別子を出力 | 実行時 `NameError`（`p2c_name_error_ref`） |
| `print(range(3))` が `<object>` | print経路に `OBJ_RANGE` なし | range 表記を追加 |

### 7.2 意味論（静かに誤っていたもの）

| 症状 | 原因 | 修正 |
|---|---|---|
| `"a" + 1`、`[1] + (2,)` 等が例外にならず強制変換 | 演算前に型検査なし | 整数/浮動/真偽以外は `TypeError`（`p2c_binop_type_error`） |
| `abs(-0.0)` が `-0.0` | 符号判定 | `+0.0` へ正規化 |
| `isinstance(x, object)` が不正C/False | `object` 未処理 | 常に True |
| 既定値が呼び出し毎に再評価（可変既定値の共有なし・`i=i` が自己参照） | 呼び出し地点へ式を展開 | **def/生成時に一度だけ評価**（モジュール関数/メソッドは隠しグローバル＋GCルート、ネスト関数/ラムダは closure env） |
| クラス属性 `C.x = C.x + 1` がリセットされる | `__classobj` が毎回クラス本体を再実行 | クラス生成時のみ本体を実行 |
| `C(kw=...)` でキーワード引数が無視 | クラス生成のKW解決なし | `__init__` のパラメータ順へ解決 |
| ユーザー `__hash__`/`__eq__` が set/dict キーで無視 | 常にポインタ同一性 | `__hash__`/`__eq__` を使用（`__eq__` のみは unhashable） |
| カスタム `__iter__/__next__` の `list()`/内包表記/アンパックが空 | 添字前提 | 反復して実体化（`p2c_iter_source` / `p2c_unpack_source`） |
| `format(1234567, ",")`、`{:#o}`、`{:#b}`、`{:_x}` が不正 | 桁区切り・代替表記未対応 | 桁区切り（3桁/4桁）と Python の `0o`/`0b` 接頭辞を実装 |
| `type(len)` が `<class 'object'>` | `OBJ_FUNCTION` 未分類 | `<class 'function'>` |

### 7.3 未対応の明示化（不正Cを出さない）

- 未定義名参照 → `NameError`。
- `*args` と `**kwargs` を混在させた呼び出し → 明示診断。
- `__init__` が可変長引数のクラスへのキーワード引数 → 明示診断。
- 捕捉した callable への可変長展開呼び出し → 明示診断。
- 複合代入の未知演算子 → 明示診断。




## 8. Round-3: 対応構文の拡張と GC の設計強化

### 8.1 GC（トレーシング・コレクタ）の設計上の穴

| 症状 / リスク | 原因 | 修正 |
|---|---|---|
| モジュール変数（ファイルスコープ変数）が 1025 個以上あるプログラムは起動時に `GC root capacity exceeded` で abort | 明示ルート表が固定配列 `P2C_GC_ROOT_CAPACITY`(1024) | 動的配列へ変更し、不足時に倍々で拡張（確保失敗時のみ診断して abort）。`p2c_gc_root_capacity()` は現在容量を返す |
| 深い入れ子データ（例: 深さ 20 万の入れ子リスト）の収集で C スタックを消費し尽くしてクラッシュ | `gc_mark()` が子を再帰でマーク | ワークリスト（`P2C_CTX->gc_work`）による**反復マーク**。ワークリストを確保できない場合のみ再帰へフォールバック |
| 巨大な文字列・リストを繰り返し作ると収集が走らずメモリが膨らむ | `gc_bytes_alloc` が `sizeof(P2C_Object)` しか計上していなかった | `p2c_malloc_checked` / `p2c_realloc_checked` / `p2c_calloc_checked` でも確保量を計上（文字列データ・配列・dict エントリ等） |
| ルート表・ワークリストがプロセス終了時に解放されない | 静的コンテキストに保持 | `p2c_runtime_shutdown()` で解放（LSan のリーク検査に適合） |
| **`join`/`sorted`/`sum` などの結果が壊れる / 実行時に落ちる（実体化配列がGCから見えない）** | `p2c_iter_items` / `p2c_range_items` が要素を **malloc した生の `P2C_Object*[]`** に入れて返していた。保守的スタックスキャンはスタック上の語を「GCオブジェクト域を指すポインタ」としてしか認識しないため、配列経由でしか参照されていない要素は収集で解放され、呼び出し元が解放後のメモリを読んでいた（収集頻度が上がるほど顕在化） | 要素を GC 追跡される `list` にも入れて生成し、**「配列ポインタがまだスタック上にある間だけルート扱いする自浄式アンカー表」**（`P2C_GcAnchor`）へ登録する。呼び出し側の変更は不要で、使い終われば次の収集で記録が自動的に破棄される |

回帰: `tests/test_gc_hardening.c`（`make test-gc-hardening`）。5000 本のルート登録、
深さ 20 万の入れ子リスト、要素 20 万のリスト、および**収集を頻発させた状態での
`join`/`sorted`（実体化アンカー）**を ASan/UBSan/LSan 付きで検証する。
この実体化アンカーの欠陥は `make test-embed-generated`（48KiB ヒープ +
しきい値 8KiB で収集が頻発する構成）で `", ".join(str(x) for x in samples)` の
結果が壊れる形で現れていた。

補足（ASan との非互換）: 本 GC は「実際のスタックを保守的に走査する」方式なので、
ASan の**偽スタック**（`detect_stack_use_after_return=1`）が有効だとフレームがヒープ上へ
移り、生存ローカルを発見できない。収集頻度の高い生成コードの差分コーパス
（`make test-asan-strict`）では明示的に `detect_stack_use_after_return=0` を設定する
（他の GC 系サニタイザ目標と同じ扱い）。また `p2c_dict_from_pairs` など
「配列で受け取った入力を構築中に参照する」ヘルパーは、入力を一時的に
`p2c_obj_incref` でピン留めして挿入中の収集に耐えるようにした。

### 8.6 Round-4: 追加構文と追加修正

| 追加 / 修正 | 内容 |
|---|---|
| リテラル内のイテラブル展開 | `[*a, *b]` / `(*a,)` / `{*a, b}` を受理（従来は構文エラー）。`(**d)` 形式の辞書マージは既存対応 |
| **ジェネレータの `send()`** | `gen.send(v)` / `gen.next()` / `gen.__next__()` を実装し、`x = yield v` が送られた値を受け取る（`next()` は None を送る）。開始前の `send(非None)` は CPython 同様 TypeError |
| **PEP 380 の戻り値** | `return value` で終わったジェネレータの `StopIteration` が `.args == (value,)` を持つ |
| 組み込み例外の `.args` | `KeyError("k").args == ('k',)`（表示は CPython 同様 repr）。`type(x).__name__`、クラスオブジェクトの `__name__` も追加 |
| ジェネレータ本体の追加文 | `assert`（メッセージ付き）と型注釈付き代入（注釈は消去）に対応 |
| 組み込み例外を基底にする `super().__init__(...)` | 例外インスタンスの `args` を設定する（従来は AttributeError） |
| ジェネレータの入れ子ループ `break` | break 時に for イテレータを破棄する（従来は外側ループから再入したとき前回の続きから回り、要素を取りこぼしていた） |
| 入れ子アンパック代入 | `(a, b), c = ...` が**黙って無視され None のまま**だったのを修正（1段しか見ていなかった）。宣言も再帰化 |
| ユーザー定義 `__lt__` によるソート | `sorted()` / `min()` / `max()` が `__lt__` を使う（従来は TypeError） |
| **`str.format(**d)` の use-after-free** | `p2c_call_attr_kw` が平坦化したキーワード配列を**使用前に解放**していた（ASan で heap-use-after-free を検出、不正な結果やクラッシュ）。解放を各経路の使用後へ移し、使用中は値をピン留め |
| **入れ子ジェネレータ式の不正C** | `sum(x for x in (y for y in ...))` で内側のステップ関数定義が外側の関数本体へ割り込み `expected expression before 'static'`。内側定義は一時バッファへ退避して `deferred_defs` として後置し、前方宣言を追加 |
| ジェネレータ式のスライス | `for d in data[:2]` を含むジェネレータ式で、パーサ合成の `p2c_obj_slice(...)` を `p2c_call(p2c_obj_slice, ...)` と出力して型エラー。内部ヘルパは直接呼び出しとして出力 |
| **反射演算子** | `2 * obj` などで `__radd__`/`__rsub__`/`__rmul__`/`__rtruediv__`/`__rfloordiv__`/`__rmod__`/`__rpow__` を呼ぶようにした |
| **単項マイナス** | `-obj` を `-1 * obj` へ展開していたため `__neg__` が無いと TypeError。`p2c_obj_neg` を追加し `__neg__` を尊重 |
| 書式 `=` アラインメント | `"{:=+9d}"` が `%=+9d` という無効な書式になっていた（符号が幅の前にあるため解析が崩れていた）。`=`（符号の後ろを埋める）と符号前置を正しく処理 |
| ジェネレータメソッドの属性/添字代入 | `self.total += v` / `self.x = v` / `d[k] = v` をジェネレータの状態機械で扱えるようにした（従来は `name op= value` 限定の診断） |
| 代入のタプル値と連鎖 | `x = 1, 2` / `a, b = c, d = 5, 6` を受理（従来はどちらも構文エラー、または後者が無言で None のまま） |
| `zip(*m)` | 展開対象が引数の列である組み込み呼び出し（転置の定番）を実行時展開で対応（`p2c_builtin_zip_star`） |
| `enumerate(x, start=1)` | キーワード `start` を尊重（従来は無視され 0 始まりになっていた） |

未対応のまま明示診断: `try`/`finally`・`with` をまたぐ `yield`、クロージャ内ジェネレータ、`bytes` リテラル、
`*args/**kwargs` を受ける callable 変数の可変長呼び出し。

### 8.7 Round-5: ビルド基準の強化と、そこで見つかった不具合

警告は「実バグを捕まえるものだけ」を採用し、ノイズ源は理由付きで除外した（根拠は Makefile の
コメントに明記）。追加した基準で、次の実在する不具合を検出して修正した。

| 追加した基準 | 検出・修正した内容 |
|---|---|
| `-Wclobbered`（setjmp/longjmp をまたぐ変数） | for/ジェネレータ反復は `P2C_SETJMP` を挟むため、反復子の受け皿が longjmp 後に不定値となり得た。生成Cの該当ローカルを `volatile` 化 |
| 生成Cへの `-Wredundant-decls` 等 | ジェネレータメソッドで前方宣言が二重出力されていた（`static P2C_Object* Class__method(...)` が2回） |
| 生成Cへの `-Wshadow=local` | 入れ子アンパック代入の一時変数名が固定で、内側の宣言が外側を影にしていた（一意な連番付与に変更） |
| `-Wstack-usage` | ホスト用ツールの `run_mode` が 46,560 バイト、GUI 側が 21,168 バイトのスタックフレームを持っていた。探索結果（約25KiB）を static へ、8KiB のログをヒープへ移し、実測最大を約8.3KiBへ削減 |
| `-Wformat-security` / `-Wbidi-chars=any,ucn` / `-Wtrampolines` | 追加時点で違反0件（非リテラル書式・Trojan Source・実行可能スタックの不在を確認） |
| `-Wstrict-aliasing=2`（`-O2`） | 追加時点で違反0件。union/キャストを多用するランタイムがエイリアス規則に適合していることを確認できたため、`-fno-strict-aliasing` は不要と判断（抑制ではなく検査を選んだ） |
| `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast` / `-Wstringop-overread` / `-Warray-compare` / `-Wsizeof-pointer-div` / `-Wmemset-*` / `-Wabsolute-value` / `-Wenum-conversion` / `-Wshift-*` / `-Wtautological-compare` / `-Winit-self` / `-Wlogical-not-parentheses` / `-Wzero-length-bounds` / `-Wflex-array-member-not-at-end` / `-Wvla-parameter` / `-Woverlength-strings` / `-Wnormalized=nfkc` | 追加時点で違反0件（境界・サイズ・引数順・符号・正規化の古典的な取り違えを今後は自動で検出） |
| 生成Cの警告基準（`GENERATED_CFLAGS`） | 本体と同じ「実バグを捕まえる」基準を成果物である生成Cにも適用。`-Wconversion` のみ、意図的な int64/size_t 変換が大量に出るため除外（理由をコメント化） |

除外した基準（無条件に厳しくしないための線引き）:

| 除外 | 理由 |
|---|---|
| `-Wc++-compat` | C++ 互換は対象外。`void*` からの暗黙変換だけで108件出る |
| `-Wnull-dereference` | 単一ヘッダ構成で全域解析になり、未チェック確保と誤検出が混在した約300件を報告する。全域のNULL契約監査が前提 |
| `-Wuseless-cast` | 「型の明示」として意図的に書いた冗長キャストを41件指摘する。可読性のための記述で、実バグ検出に寄与しない |
| `-Wdeclaration-after-statement` | C11 では宣言と文の混在は合法。C89スタイルの強制は言語仕様に反する |
| `-Wsuggest-attribute=const,pure` / `-Wunsafe-loop-optimizations` | 性能ヒントであり正しさの検査ではない。`make test-opt-hints`（違反しても失敗しない読み物として）へ分離 |
| `-Wshadow`（生成Cに global まで） | Python では関数引数がモジュール変数を隠すのは正常なスコープ規則。生成Cのみ `-Wshadow=local` に限定 |

さらに、追加した構文・修正:

| 追加 / 修正 | 内容 |
|---|---|
| `assert cond, expr` | リテラル以外のメッセージ式も評価し、`AssertionError.args` に載せる（従来は "assertion failed" に置き換わり、式の副作用も落ちていた）。ジェネレータ内 assert も同様 |
| `dict` の `\|` / `\|=` | CPython 3.9 以降と同じ「右優先のマージ」であることを回帰テストで固定（実装は既存） |
| `str.removeprefix` / `str.removesuffix` | 実装済みであることを回帰テストで固定 |
| スタック予算の回帰ゲート | `make test-stack-budget` が組込みコアを 4096 バイト予算で再ビルドし、余裕を数値で担保する |
| 生成Cの品質ゲート | `make test-conformance` が生成Cを `GENERATED_CFLAGS`（本体と同等の警告基準）でコンパイルする |


### 8.8 Round-6: 全体精査で見つけた不具合と、初心者向けの入口整備

`src/` と `include/` の全ファイルを対象に、差分コーパス（新規 16 ケース）と
GCC `-fanalyzer` / サニタイザ / 厳格ビルドで精査し、次を修正した。

| 修正 | 症状 / 内容 |
|---|---|
| **式評価スタックの動的化** | `p2c_binop_begin`/`p2c_fstr_begin` が固定長（64/32 段）だったため、**再帰呼び出しを含む式**（`return 1 + f(n-1)` を 200 段）で「binary expression nesting limit exceeded」を誤って送出し、実行時 abort になっていた。必要に応じて伸長する動的スタックへ変更（中身は従来どおり GC がルートとして走査し、`p2c_runtime_shutdown` で解放） |
| **入れ子 for ターゲットの無言 None** | `for (a, (b, c)) in ...` で内側の変数が `None` のままだった（タプル 1 段しか見ていなかった）。`gen_for_bind_from_expr` に一本化し、入れ子・starred を再帰的に束縛。属性/添字など未対応形は位置つき診断へ |
| **括弧付き for ターゲットの構文エラー** | `for (p, q) in ...` / `for (x) in ...` が「expected loop variable name」で拒否されていた。括弧付きは通常のアンパック用パーサで読むようにした |
| **`test-sanitizers` が既定ビルドを破壊** | サニタイザ目標が冒頭で `make clean` を呼び、通常ビルドの `build/` を消していた（他目標の成果物を壊す）。サニタイザ専用ディレクトリのみを掃除するよう修正 |
| クロージャの捕獲仕様を明文化 | 捕獲は「値」であり Python の遅延束縛と異なる（`for i in ...: lambda: i` は `0,1,2`）。`lambda i=i: ...` で固定する旨を仕様書・入門ガイドに明記 |

さらに、初心者向けの入口を整備した。

| 追加 | 内容 |
|---|---|
| `py2c help` | 引数なし・`help`/`Help`/`HELP`/`--help`/`-h`/`-?`/`usage` で、**使い方・主なオプション・はじめの一歩・自分でビルドする手順・つまずいたときの切り分け**を表示する（大小文字を無視） |
| `--version` | バージョンだけを表示する |
| 誤り時の案内 | 不明なオプションや存在しない入力ファイルで `help` への導線を表示（終了コード 2） |
| `make py2c` | `build/py2c` と `bin/py2c` を用意（`./build/py2c help` がそのまま使える） |
| `docs/GETTING_STARTED_ALPHA1.0.md` | 変換の流れ、3 コマンドでの動作確認、オプションの使い分け、切り分け手順、知っておくべき仕様（GC・setjmp・クロージャ）をまとめた入門ガイド |

`*args/**kwargs` を受ける callable 変数の可変長呼び出し。

### 8.9 Round-6: ビルド基準とリンク時硬化の追加

| 追加 | 根拠 |
|---|---|
| `-fno-common` | 一時定義のマージを禁じ、シンボル重複をリンク時に検出する（GCC10+ の既定を明示化し環境差をなくす） |
| `HOSTED_LDFLAGS`（Linux のみ自動適用） | ホスト CLI/GUI に `-Wl,-z,relro,-z,now,-z,noexecstack` を付与。GOT 書き換えとスタック実行を防ぐ。macOS 等では自動的に無効化し、`make HOSTED_LDFLAGS=...` で上書き可能 |
| `make py2c` | 初心者向けの短い入口（`build/py2c`）を用意 |


### 8.2 契約違反（実行時 abort を出していた未対応構文）

| 症状 | 原因 | 修正 |
|---|---|---|
| ループ内に `yield` があると、変換は成功するのに生成物が実行時に `NotImplementedError: suspension inside this statement is not yet supported` で abort | 状態機械がトップレベル文しか扱えず、未対応時に**実行時 raise のC**を生成していた | 変換時点の位置つき診断（`codegen_set_error_at`）へ変更。診断には文の種別（`stmt_kind_label`）と line/col を含める |
| 未対応構文の診断が `unsupported construct encountered during code generation` のみで、何が原因か分からない | 構文名・位置を含めていなかった | `line L, col C: unsupported construct: <種別> (<対処>)` 形式へ統一 |
| クロージャ内のジェネレータの診断が `expression` | yield 専用の分岐が無かった | `yield in this position` と明示 |

### 8.3 黙って誤った結果を出していた `str.format` / f-string

| 症状 | 原因 | 修正 |
|---|---|---|
| `"{0[0]}-{0[2]}"` が `[1, 2, 3]-[1, 2, 3]` | フィールド全体（`0[0]`）を位置インデックスとして解釈 | アクセサ列 `[i]` / `[key]` / `.attr` を適用 |
| `"{d[k]}"` が `KeyError: 'd[k]'` | アクセサを含む名前をキーワード引数名として検索 | ベース名（`d`）で解決してから添字アクセス |
| `"{v!r}"` が `KeyError: '!r'` | 変換指定が未処理 | `!r` / `!s` / `!a` を実装（`p2c_obj_repr` / `p2c_obj_str`） |
| `"[{:>{}}]".format("ab", 3)` が `[ab}]` | 書式指定中の入れ子フィールドを解釈せず、閉じ括弧も誤検出 | 入れ子フィールドを値で置換（`format_field_value` を再帰、深さ上限あり）。フィールド抽出も入れ子の `{}` を数える |
| `f"{name:>{w}}"` が幅を無視 | f-string の書式を静的文字列として渡していた | 書式を実行時に組み立てる式へ変換（`fstr_build_spec_expr`）。入れ子フィールド自身が書式を持つ場合も再帰的に処理 |
| 位置インデックスが範囲外でも `None` を表示 | 範囲検査なし | `IndexError: Replacement index out of range`（CPython と同じ） |

### 8.4 追加した構文

| 追加 | 内容 |
|---|---|
| **ジェネレータ関数のループ内 `yield`** | `for` / `while` / `if` / `elif` / `else` / `break` / `continue` / ループの `else` 節 / 入れ子ループ / `yield from` / `x = yield v`（`send()` 対応。再開時に送られた値を受け取り、`next()` は None を送る）に対応。状態機械は**ドライバループ**（`for(;;) switch(state)` ＋ `set_state; continue;`）で生成し、状態遷移で C スタックを消費しない |
| **ジェネレータメソッド** | `class` の内側の `def` が `yield` を持つ場合（従来は未対応）も同じ状態機械で生成 |
| **クラスからのメソッド取り出し** | `A.staticmethod` / `A.classmethod` / `A.instance_method`（`A.m(inst, x)` も可）。従来は `AttributeError` |
| **型オブジェクトによる判定** | `isinstance(x, type(None))` / `isinstance(x, type([]))` / `issubclass(C, type(None))`。`type()` は本ランタイムでは `<class 'Name'>` 文字列を返す近似実装のため、その文字列も型名として解決する（`p2c_isinstance_of_typeobj` / `p2c_issubclass_of_typeobj`） |

未対応のまま**明示診断**するもの（不正なCや実行時 abort にはならない）:
`try`/`finally` をまたぐ `yield`、`with` をまたぐ `yield`、クロージャ内で定義した
ジェネレータ（捕捉変数）、`bytes` リテラル、属性・添字への代入を伴うジェネレータ本体。

### 8.5 検証

- `tests/round3_alpha10.py`（`tests/conformance_regression.sh` の R01–R29）:
  format フィールド / 入れ子書式 / f-string、メソッド取り出し、`type(None)`、
  ジェネレータのループ・`break`/`continue`/`else`/`yield from`/メソッド を CPython と差分比較。
- `make test-gc-hardening`: ルート表の動的拡張と反復マークの回帰（ASan/UBSan/LSan）。
- 総アサーション数: 803 → 832。

### 8.10 Round-9: 整数比較の精度落ち（CPython と結果が食い違っていた）

| 項目 | 内容 |
|---|---|
| 症状 | `9223372036854775806 < 9223372036854775807` が **False**（CPython は True）。`>=` は逆に True |
| 原因 | 順序比較 `p2c_obj_order_cmp()` が str・list/tuple 以外をすべて `double` へ変換して比較していた。int64 を double にすると 2^53 を超える桁が丸められ、隣接する 2 つの整数が同じ値になる |
| 修正 | int 同士は int64 のまま厳密に比較する分岐を追加（float や bool が混ざる場合は従来どおり数値比較） |
| 発見の経緯 | AOT アンボクシング用の生 int 演算ヘルパ（`p2c_int_*_raw`）のテスト `tests/test_int_raw_ops.c` が「P2C_Object* 版と結果が一致しない」として検出。境界値（2^53 超・INT64_MAX/MIN）を含む値のマトリクスで全演算子を突き合わせている |
| 回帰 | `make test-int-raw-ops`（値マトリクスの全演算子 × 例外名の一致）と、CPython 差分（`9007199254740993 > 9007199254740992` 等）で固定 |

> `==`/`!=` は元から厳密だったため、この不具合は順序比較（`< <= > >=`）だけに
> 出ていた。2^53 を超える整数を比較するプログラムでのみ再現する。

