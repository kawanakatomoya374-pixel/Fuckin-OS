# Python Code to C Alpha1.0 — 機能リファレンス

このドキュメントは「**どの Python の構文・機能が、どう C に対応するか**」を一覧する
リファレンスです。内容は実装と `./build/py2c --supported` の出力を正とし、未対応のものは
**位置付きの変換エラー（または明示的な互換フォールバック）**になることを契約としています。

- はじめの一歩: [`../GETTING_STARTED_ALPHA1.0.md`](../GETTING_STARTED_ALPHA1.0.md)
- リリースノート: [`../release/ALPHA1.0_RELEASE_NOTES.md`](../release/ALPHA1.0_RELEASE_NOTES.md)
- 不具合修正の記録: [`../review/BUGFIX_ALPHA1.0.md`](../review/BUGFIX_ALPHA1.0.md)

## 1. 文

| 構文 | 対応状況 |
|---|---|
| `if` / `elif` / `else`、`while`（`else` 節含む）、`for ... else` | 対応 |
| `for <var\|tuple\|starred> in <iterable>`（括弧付き・入れ子ターゲット含む） | 対応 |
| `def`（デフォルト引数・キーワード引数・`*args`・`**kwargs`・キーワード専用 `*`） | 対応 |
| `return`（複数値のタプル戻り値含む）、`pass`、`break`、`continue`、`global` | 対応 |
| `class`（メソッド、`__init__`、継承、`super()`、`@staticmethod`/`@classmethod`/`@property`、decorator） | 対応 |
| `try` / `except` / `else` / `finally`、bare `raise`、`raise ... from ...` | 対応 |
| `match` / `case`（literal・None・capture・wildcard・sequence/mapping/class・as・star・or・guard） | 対応 |
| `import <mod>` / `from <mod> import <name>`（`math`、`pygame` ヘッドレス） | 対応 |
| 代入（`=`）、代入式（`x := v`）、複合代入（`+=` など。属性・添字ターゲット含む） | 対応 |
| タプル/Starred unpack 代入（`a, *mid, z = seq`）、複数代入（`a = b = c = 1`）、セミコロン区切り | 対応 |
| `assert`（`assert cond, msg` の msg 式も評価して `AssertionError.args` に載せる） | 対応 |
| `with`（複数コンテキストマネージャ） | 対応 |
| `yield` / `yield from` / `send()`（`for`・`while`・`if` の**内側**にある `yield` を含む） | 対応（状態機械へ変換。`try`/`with` をまたぐ `yield` は未対応） |
| `async def` / `await` / `async with` / async generator | 協調スケジューラ前提で対応（`docs/build/ASYNC_*` 参照） |

## 2. 式

| 構文 | 対応状況 |
|---|---|
| 数値（int/float）・文字列・bool・None・`...`（Ellipsis） | 対応 |
| `list` / `dict` / `tuple` / `set` リテラル、隣接文字列リテラルの暗黙連結 | 対応 |
| リテラル内展開 `[*a, *b]` / `(*a,)` / `{*a, b}` / `{**d}` | 対応（strict C11 では診断） |
| list / dict / set 内包表記、generator expression（複数 `for` 節・タプルターゲット・`if` フィルタ） | 対応 |
| 算術・比較・論理・集合演算子、連鎖比較（`1 < x < 10`） | 対応 |
| dict マージ `d1 \| d2` / `d1 \|= d2` | 対応 |
| 三項式（`x if c else y`） | 対応 |
| f-string（`f"{x:>8}"`、`{x!r}`、入れ子の書式指定、`=` アラインメント） | 対応 |
| `lambda`（デフォルト引数含む） | 対応（捕獲は**値**。下記「クロージャ」参照） |
| 添字・スライス・属性アクセス、list スライスの代入・`+=`・`del` | 対応 |
| 呼び出しのキーワード引数、`f(*lst)` / `f(**d)`（既知の関数）、`zip(*m)` | 対応 |
| タプル値代入 `x = 1, 2`、連鎖代入 `a, b = c, d = 5, 6` | 対応 |
| ユーザー定義 `__lt__` による `sorted()`/`min()`/`max()` | 対応 |
| 反射演算子（`__radd__` 等）と単項 `__neg__` | 対応 |

## 3. 組み込み

| 種別 | 対応 |
|---|---|
| 関数 | `print`（`sep=`/`end=`）、`len`、`range`、`input`、`str`、`int`、`float`、`bool`、`abs`、`round`、`min`、`max`、`sum`、`sorted`（`key=`/`reverse=`）、`enumerate`（`start=`）、`zip`、`isinstance`（型タプル・クラスオブジェクト）、`issubclass`、`type`、`any`、`all`、`map`、`filter`、`list`、`tuple`、`set`、`dict`、`divmod`、`pow`（3 引数）、`format`、`callable`、`repr`、`ord`、`chr`、`bin`、`oct`、`hex`、`iter`、`next`、`hasattr`/`getattr`/`setattr`/`delattr`、`hash` |
| `list` | `append`、`pop`、`insert`、`remove`、`count`、`index`、`extend`、`clear`、`reverse`、`sort`、`copy` |
| `dict` | `get`、`keys`、`values`、`items`、`update`、`pop`、`popitem`、`setdefault`、`clear`、`copy` |
| `set` | `add`、`discard`、`remove`、`clear`、`copy`、`union`、`intersection`、`difference`、`symmetric_difference`、`update` 系、`isdisjoint` |
| `str` | `upper`/`lower`/`casefold`/`capitalize`/`swapcase`/`title`、`strip` 系、`split`、`join`、`replace`、`find`/`index`/`rfind`/`rindex`、`count`、`startswith`/`endswith`、`removeprefix`/`removesuffix`、`format`、`center`/`ljust`/`rjust`/`zfill`、判定系（`isalpha`/`isdigit`/…） |
| 書式指定 | `{:05d}` `{:.2f}` `{:>10}` `{:x}` `{:b}` `{:,}` `{:.0%}` `{:=+9d}` などの主要な書式 |
| 特殊メソッド | `__init__`、`__str__`、`__repr__`、`__eq__`、`__hash__`、`__lt__`、`__len__`、`__bool__`、`__iter__`（ジェネレータ）、`__getitem__`/`__setitem__`/`__contains__`、算術系（`__add__`/`__mul__`/`__sub__`/…と反射版） |
| 例外 | 組み込み例外の構築・`.args`・`type(x).__name__`、`super().__init__(...)`（組み込み例外基底）、`raise ... from`、`. __cause__` |
| モジュール | `math`（`pi`/`e`/`tau`/`inf`/`nan` と主要関数）、`pygame`（ヘッドレス: 実描画・音声・入力なし） |

## 4. 数値・意味論の仕様

- 整数は **64 ビット固定**。範囲外は `OverflowError`（任意精度整数は非対応）。
- `//`・`%`・`divmod` は Python と同じ「床除算」の符号規則（`-7 // 2 == -4`）。
- `round()` は偶数丸め（banker's rounding）。
- 真偽値は Python と同じ規則（空のコンテナ・`0`・`""`・`None` は偽）。
- `min`/`max`/`sorted` は `__lt__` を尊重し、系列（tuple/list）の辞書式比較にも対応。

## 5. ランタイムの仕様

- **GC は保守的（conservative）**: スタック上に置かれたポインタも到達可能とみなす。C 側で
  ポインタを配列などスタックから見えない場所へ置く場合は、ランタイムのアンカー機構
  （`p2c_iter_items` が自動で行う）やルート登録を使う。
- **例外は setjmp/longjmp**: `try` をまたいで変更する自動変数は `volatile` が必要
  （生成コードは自動で付与）。式評価用の一時スタックは、ハンドラ到達時に
  `p2c_binop_rewind()`/`p2c_fstr_rewind()` で巻き戻す。
- **ジェネレータは状態機械**: ループを含むジェネレータは `for(;;) switch(state)` の
  ドライバループへ変換され、状態遷移で C スタックを消費しない。
- **クロージャは値を捕獲**: Python の遅延束縛とは異なる。ループ変数を固定するには
  `lambda i=i: ...`。
- **スタック予算**: 組込みコアの 1 フレームは 4096 バイト以内（`make test-stack-budget` が検証）。
- **不変の静的リテラル**: 文字列リテラルと、識別子名・キーワード名・辞書キーなど
  コンパイル時に内容が確定する文字列は、`P2C_STATIC_STR` マクロで定義した
  「共有・不変・GC 非追跡・コンテキスト非依存」の静的オブジェクトになる。文字列は
  `.data` 上の書き込み可能配列に置かれ、実行時の確保・GC 負荷はゼロ。同じ内容は
  モジュール内で 1 つに共有される（`is` の同一性は CPython のリテラル intern と同様に真）。
  プログラムに現れる相異なるリテラルの数だけ静的領域を消費する点に注意。
- **確保統計（測定用・ホスト構成のみ）**: `P2C_ALLOC_STATS=1` を付けて実行すると、
  終了時に stderr へ `p2c_obj_from_str` の呼び出し回数、オブジェクトの新規確保/再利用
  回数、GC 収集回数、ピーク時のオブジェクト数を出力する（既定は無効）。
- **ランタイムインスタンス**: 既定は 1 プロセス 1 インスタンス。カーネルのタスクごとに
  独立した実行環境を持つ場合は `p2c_runtime_context_create()` /
  `p2c_runtime_context_select()` / `p2c_runtime_context_destroy()` でインスタンスを
  切り替える（GC・クラス/モジュールレジストリ・フリーリスト・式スタックが分離される。
  詳細と注意点は `docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md` §5-2）。

## 6. 既知の制限（明示的に診断するもの）

| 制限 | 挙動 |
|---|---|
| `try`/`finally`・`with` をまたぐ `yield` | 変換時に位置付きエラー |
| クロージャ内で定義したジェネレータ | 変換時に位置付きエラー |
| `bytes` / `bytearray` リテラル・型 | 変換時に位置付きエラー |
| callable 変数への `*args`/`**kwargs` 可変長呼び出し | 変換時に位置付きエラー（既定）／`--fallback` で実行時エラー |
| 任意精度整数 | 実行時に `OverflowError` |
| 数値・文字列の完全な CPython 互換（例: 巨大整数の桁数、`repr` の細部） | 差異あり（コーパスで検出したものは修正済み） |

未対応の構文は、既定では**変換エラー**として報告します。`--fallback` を付けると
「実行時に `NotImplementedError` を送出するコード」へ置換して変換を続けるため、
まず全体を通して動かしたいときに便利です。

## 7. 検証

- CPython 差分コンフォーマンス: **868 アサーション**（`make test-conformance`。コア
  C01–C770、バグ修正回帰 F01–F33、Round 3–6 の回帰 R01–R65）
- 生成 C は `GENERATED_CFLAGS`（本体と同等の警告基準）で全件コンパイルする
- サニタイザ（ASan/UBSan）、GCC `-fanalyzer`、clang 静的解析、単一ヘッダー
  （C11/C99/freestanding/TinyCC）、組込み・自作OS テンプレート、スタック予算の検証を
  `make test` が一括実行する
