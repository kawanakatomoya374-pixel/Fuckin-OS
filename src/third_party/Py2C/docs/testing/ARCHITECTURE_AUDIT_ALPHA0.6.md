# アーキテクチャ監査（Alpha0.6）

指摘された7項目について、**推測ではなく測定値**で現状を固定し、優先順位と次の一手を記録する。
再測定は `sh tests/quick_diff_alpha06.sh tests/compat_probe_alpha06.py` と
`sh tests/verify_targets_alpha06.sh <target>...` で行える。

## ① Python 互換性の穴 / ⑦ CPython と結果が違うバグ

`tests/compat_probe_alpha06.py`（80項目）を CPython と差分比較した結果。

| 項目 | CPython | 本実装 | 種別 | 優先 |
| --- | --- | --- | --- | --- |
| P19/P20/P21 大きな整数（`2**100`、`10**30`、(2**64)//3） | 任意精度で成功 | `OverflowError`（64bit のみ） | 既知の設計制限（要明記） | 高（設計判断） |
| P26 `int("0x1f", 16)` | 31 | 0 | **バグ**（基数指定時の 0x/0o/0b 前置の扱い） | 中 |
| P48 `repr("a'b")` | `"a'b"`（二重引用符を選ぶ） | `'a\'b'` | **バグ**（repr の引用符選択） | 低（表示のみ） |
| P63 `(1, 2) < (1, 3)` | True | False | **バグ**（タプルの辞書式比較） | 高（sorted/min/max に影響） |
| P80 `isinstance(True, int)` | True | False | **バグ**（bool ⊂ int の継承関係） | 中 |

**今回の修正（このセッションで発見・修正済み）**

| 症状 | 原因 | 修正 |
| --- | --- | --- |
| `pow(a, b)`（2引数）が生成Cのコンパイルに失敗（存在しない `p2c_user_pow` を呼ぶ） | codegen が3引数のモジュラべき乗のみ特別扱いし、2引数が汎用呼び出しへ落ちていた | `argc == 2` を `p2c_obj_pow` へ割り当て |
| `min(..., key=lambda v: ...)` 等、**ネストしたラムダ**が壊れたCになる（`static` 定義が式の途中に埋め込まれる） | ラムダ/ジェネレータ/関数本体は `forward` バッファへ書くが、`forward` 書き込み中に現れたラムダが同じバッファへ割り込み、後方参照も未宣言だった | ネスト定義は別バッファへ書き「待ち行列」に積み、最終組み立てで `forward` 末尾へ連結。あわせて `header` に前方宣言を出力 |

**パース段で弾かれる（＝互換性の穴）**: `(lambda a, b=2: a + b)(1)` のような「呼び出し式の中のラムダ（既定値付き）」、
および「ラムダ内のジェネレータ式（ネストしたクロージャの捕捉）」。
これらは現状 `P2C_ERR_NOT_IMPLEMENTED` の明示診断で止まる（`--fallback` で回避可能）。

## ② 値が全て `P2C_Object*`（ボクシング）で重い

- すべての値がヒープオブジェクト。`p2c_obj_from_int` の結果も同様で、`a + b` は**必ず1個以上確保**する。
- 小整数キャッシュは `P2C_SMALL_INT_COUNT`（256 個）に限定。範囲外は毎回確保。
- 測定手段は既にある: `p2c_sandbox_allocs()` / `p2c_gc_stats()`（確保数・収集回数）。
- 次の一手: (a) 小整数キャッシュ拡大と `-5..256` の一致、(b) ループ内の一時値に対して
  「スタック値（タグ付き union）」を使う最適化パス、(c) 文字列連結の in-place 化。

## ③ グローバル状態依存（組み込み・並行実行に弱い）

- `src/runtime/python_code_to_c_runtime.c` のファイルスコープ可変グローバルは **37 個**。
  代表: GC（`g_gc_all`/`g_gc_threshold*`/`g_gc_roots`…）、例外スタック（`p2c_exc_stack`）、
  クラスレジストリ、モジュールレジストリ、サンドボックス予算。
- 影響: マルチコア/割り込みで共有すると GC・例外・名前解決が壊れる。単一タスク前提。
- 次の一手: 値はすべて `P2C_RuntimeCtx` へ集約し、`p2c_runtime_ctx_get()` 経由にする
  （スレッド/TLS 対応の第一歩）。当面は「単一タスク前提」を明記＋`p2c_sandbox_reset()` 等の
  タスク開始/終了 API を揃える。

## ④ 固定上限が多い

| マクロ | 値 | 超過時の挙動 |
| --- | --- | --- |
| `P2C_MAX_CLASS_REGISTRY` | 256 | **黙って登録しない**（要警戒） |
| `P2C_MRO_MAX_BASES` / `_DEPTH` / `_NAMES` | 8 / 12 / 32 | C3 を諦めて深さ優先へ（順序が変わりうる） |
| `P2C_ASYNC_QUEUE_CAPACITY` | 256 | 同時コルーチン数の上限 |
| `P2C_BINOP_STACK_CAPACITY` / `P2C_FSTR_STACK_CAPACITY` | 64 / 32 | 評価スタックの上限 |
| `P2C_GC_ROOT_CAPACITY` | 1024 | 追加ルート数（超過時は要確認） |
| `P2C_SMALL_INT_MAX` | 256 | 小整数キャッシュの範囲 |

次の一手: (a) 超過時に**失敗を通知**（例外/診断）する、(b) すべて `-D` で上書き可能にし
`#ifndef` ガードを付ける、(c) レジストリは動的配列へ。

## ⑤ C 生成器が複雑

- `src/codegen/python_code_to_c_codegen.c` は **4,526 行**（ランタイム 6,360 行）。
  `gen_expr`/`gen_stmt` が巨大な switch で、`cg->current`（header/forward/body/toplevel）の
  切り替えが暗黙の状態になっている（今回のネスト定義バグの温床）。
- 次の一手: (a) 出力バッファ切替を `cg_emit_start/end(buf)` に集約して明示化、
  (b) `gen_expr` を組み込み型/呼び出し/演算子/リテラルへ分割、
  (c) 生成結果のスナップショットテスト（現在は CPython 差分が実質その役割）。

## ⑥ ドキュメントと実装の整合性

- 本セッションで 576 → **749** アサーションへ更新（README / リリースノート / 機能リファレンス /
  適合マトリクス / テストレポート）。
- 次の一手: `make help` の全ターゲットと実ターゲットの一致を検査するスクリプト化
  （`tests/run_all_make_targets.sh` が実質の検査。失敗時は要修正）。

## ①/⑦ 追補：Unicode（コードポイント意味論）と変換の厳密化

### 修正済み（本セッション後半）

| 症状 | 原因 | 修正 |
| --- | --- | --- |
| `len("é")` = 2、`"é"[0]` が UTF-8 の断片、`ord(s[0])` が失敗 | 文字列が UTF-8 バイト列のまま添字・長さ・反復されており、バイト単位で公開していた | UTF-8 ヘルパ（`p2c_utf8_count/offset/step`）を追加し、`len`・`s[i]`・スライス・反復・`find`/`index`/`rfind`/`rindex`・`count`・`startswith`/`endswith` の境界と戻り値を**コードポイント単位**へ統一 |
| `int("123x")` = 123、`float("1.2x")` = 1.2 | `strtoll`/`strtod` の未消費末尾を見ていなかった | 文字列全体の消費を要求（`ValueError`） |
| `int("9223372036854775808")` が飽和、`int("-9223372036854775809")` が飽和 | 範囲外を無視して飽和値を返していた | 手書きの範囲検査で **`OverflowError`**（`errno` 非依存なので freestanding でも同じ） |
| `x in dict`/`x in set` が全バケット走査（実質 O(n)） | ハッシュを使わず線形探索していた | 挿入側と同じハッシュで 1 バケットに限定（平均 O(1)） |

回帰: `C594–C615`（変換の厳密性 20 件）と `C616–C749`（Unicode 22 件）を追加。

### Unicode 対応の範囲（現状）

| 対応済み | 未対応（要対応） |
| --- | --- |
| `len` / 添字（負含む）/ スライス（step・逆順含む）/ 反復 / `ord` / `chr` / `in` / `find`・`index`・`rfind`・`rindex`（コードポイント位置）/ `count` / `startswith`・`endswith` の start/stop / `upper`・`lower`・`casefold`・`capitalize`・`swapcase`・`title`（Latin-1 / Latin Extended-A / Greek / Cyrillic / Fullwidth をアルゴリズム変換）/ `<`・`<=`・`>`・`>=` と `sorted`/`min`/`max` のコードポイント順 / `center`・`ljust` の幅（コードポイント数） | `%`・`format` の幅指定（printf がバイト数で数える）/ `\N{...}` 名前エスケープ / BMP 外や多対一の大小（`ß`→`SS`、`İ`→`i̇` など）/ `rjust` の幅 / `casefold` の完全版 |

次の一手: Unicode の大小変換表（BMP のみでも）と、`sorted`/比較をコードポイント順へ切り替える
（後者は `p2c_obj_lt` の文字列比較を `memcmp` からコードポイント比較へ）。

## ② / ④ 追補：データ構造と上限

- `dict`/`set` の動的 resize を**実装済み**（本セッション）。`bucket_count` は 32 から始まり、**負荷率 75% を超えると 4 倍へ rehash**（`p2c_hash_rehash` が `order_next` を辿って張り直すため挿入順はそのまま）。上限は 1<<22 バケット。
  - 実測（20000 キーの dict 構築 + 20000 回の `in`、20000 要素の set + 20000 回の `in`）: **修正前 1.18 s → 修正後 0.10 s（約 12 倍）**。手動ベンチは `tests/dict_bench_alpha06.py`。
  - 正しさの回帰: `C648–C749`（2000 キーの dict と 1500 要素の set で、順序・参照・削除が CPython と一致）。
- クラスレジストリ（256）は**黙って登録しない**ため、上限到達を例外/診断にする必要がある。
- GC ルート（1024）超過も同様に失敗を通知する。
- 全上限は `-D` で上書き可能にし、`#ifndef` ガードを付ける。


```
make test-conformance      749 assertions
make test-c99              749 assertions（厳格C99）
make test-tcc              749 assertions（TinyCC）
make test-analyzer         欠陥0 / make test-sanitizers-core PASS
make test-sandbox / test-embed-runtime / test-gc-adaptive / test-hobby-os-template PASS
```

### 固定上限の診断（本セッション）

固定長配列の表（クラスレジストリ、MRO の作業領域、GC ルート、async キュー）は、
以前は上限に達すると *黙って* 処理を飛ばしていた（クラス登録の欠落は `super()` が
別クラスを呼ぶ等の誤動作として後から現れる）。現在は `p2c_limit_exceeded()` が
同じ上限について一度だけ `p2c: limit exceeded: <名前> (limit=<n>, override with -D<マクロ>)`
を出力し、実行中で安全な場所では `RuntimeError` も投げる。上限はすべて
`-DP2C_xxx=N` で上書きできる（表は `docs/spec/SYNTAX_AND_PORTABILITY.md`）。

`make test-limits` が 4 ケース（既定 / レジストリ絞り / MRO 絞り / 上限拡大）を検証し、
「無言の不一致を出さない」ことを保証する。

### 可変グローバルの集約（本セッション・TLS/スレッド化の準備）

可変な状態がファイルスコープのグローバルに散在していた（クラスレジストリ 3 + async
キュー 4 + 実行中フラグ 1 + 上限通知履歴 2 = 10 個）。これを `P2C_RuntimeContext`
構造体へ集約し、**可変グローバルはコンテキストポインタ 1 本のみ**にした。

- 本体は `P2C_CTX->...` 経由でアクセスする（`#define P2C_CTX (g_runtime_context)`）。
- 将来 TLS 化するときは `g_runtime_context` をスレッドローカルへ置き換え、
  インタプリタごとにコンテキストを割り当てればよい（残りの `g_gc_*` /
  `g_sandbox_*` / `g_oom_*` も同じ構造体へ段階的に移す）。
- 観測用に `size_t p2c_runtime_class_count(void)` を追加。`p2c_runtime_shutdown()` で
  0 に戻ることを `tests/test_gc_runtime_reinit.c`（3 エポック）が検証する。

#### 第2段: GC / サンドボックス / OOM ハンドラ（+33）

続けてコレクタと予算管理の状態も同じ構造体へ移した（合計 **43 個**、書き換え 194 箇所）。

| 群 | 移した状態 |
|---|---|
| GC（24） | `gc_all`, `gc_roots[1024]`, `gc_root_count`, `gc_collecting`, `gc_stack_bottom/lo/hi`, `gc_enabled`, `gc_threshold`, `gc_threshold_base/max`, `gc_adaptive`, `gc_threshold_growths`, `gc_peak_objects`, `gc_oom_resets`, `gc_bytes_alloc`, `gc_collections`, `gc_last_freed`, `gc_obj_count`, `gc_scan_warned`, `gc_addr_lo/hi`, `gc_scan_words`, `gc_temp_roots` |
| サンドボックス（5） | `sandbox_max_ticks`, `sandbox_max_allocs`, `sandbox_ticks`, `sandbox_allocs`, `sandbox_violations` |
| OOM（4） | `oom_handler`, `oom_user`, `oom_exception`, `oom_in_handler` |

非ゼロの初期値（`gc_enabled = true`, `gc_threshold = 256KiB`, `gc_threshold_base`,
`gc_adaptive = true`, `gc_threshold_max`, `gc_addr_lo = (uintptr_t)-1`）だけを
designated initializer で明示し、残りは 0（従来の既定値と同一）。

#### 第3段: モジュールレジストリとスキャン索引（+4）

`module_registry`（import 済みモジュール名 → モジュール、16 箇所）と、
保守的スタックスキャンの一時索引 `scan_index` / `scan_index_cap` / `scan_index_used`
（27 箇所）も同じ構造体へ移した。**合計 47 個、書き換え 237 箇所**。

残るファイルスコープの可変状態は次の 6 つだけで、いずれも意図的に残している:

| 名前 | 理由 |
|---|---|
| `g_small_ints` / `g_small_ints_ready` | 初期化時に一度だけ構築し、以後は不変（全インタプリタで共有してよいキャッシュ） |
| `g_binop_stack` / `g_binop_depth` | 既に `P2C_THREAD_LOCAL`（スレッドごとの二項演算の作業領域） |
| `g_fstr_stack` / `g_fstr_depth` | 既に `P2C_THREAD_LOCAL`（f-string の作業領域） |
| `g_runtime_context_default` / `g_runtime_context` | コンテキスト本体と、その唯一のポインタ |

つまり「インタプリタごとに持つべき状態」はすべて `P2C_RuntimeContext` に入った。
スレッド対応の次の一歩は、`g_runtime_context` を `P2C_THREAD_LOCAL` 化し、
スレッド開始時に `p2c_runtime_init()` でコンテキストを与えることだけになる。

### range() の遅延化（本セッション・項目「range() の遅延化」）

以前の `p2c_range()` は**要素数分のリストを実体化**していた（`range(10**9)` のループは
メモリを食い尽くして事実上ハングし、`step == 0` も黙って 1 に化けていた）。
現在は `(start, stop, step)` だけを持つ `OBJ_RANGE` を返し、要素は算術で求める。

| 操作 | 実装 |
|---|---|
| `len(r)` | `1 + (span-1)/|step|`（符号なし計算で桁溢れ回避）。int64 超は `OverflowError` |
| `r[i]` / `for` | `p2c_subscript_get` に算術経路（負の添字、範囲外は `IndexError`）。`p2c_iter_at` 経由で `for` も同じ |
| `x in r` | 算術判定（`1.0 in range(3)` は True、`1.5` は False） |
| `repr`/`str` | `range(0, 5)` / `range(1, 5, 2)`（`p2c_obj_to_buf_ex` の早期分岐なので print/f-string/repr が同一） |
| `==` | CPython 準拠で **(長さ, start, step)**。要素が無ければ start/step に関わらず等しい（`range(0) == range(1, 1)`）。リストとは常に不一致 |
| `list()`/`sorted()`/`sum()`/`min`/`max` | `p2c_iter_items` に展開経路（巨大 range は `MemoryError`） |
| `bool(r)` | 空なら偽（`p2c_obj_is_truthy` に算術判定を追加） |
| `iter(r)` | `p2c_builtin_iter` の系列ケースに追加 |

引数の型は厳密化した: `range(1.5)` は `TypeError: 'float' object cannot be interpreted as an
integer`（CPython と同一文言）、`range(0, 5, 0)` は `ValueError: range() arg 3 must not be zero`。

回帰: `C658–C749`（`tests/range_lazy_alpha06.py`、23 行が CPython と一致。10**12 の range を
len/添字/in/repr で扱うため、遅延化が崩れれば必ず落ちる）。

既知の未対応: **スライス**（`range(10)[2:5]` は CPython では `range(2, 5)` を返すが、
現在は未対応）。以前はリストだったため動いていた経路であり、次の作業で実装する。
`hash(range)`（辞書キー）も未対応。

#### range のスライスと hash（同セッションで完了）

- `p2c_obj_slice()` に `OBJ_RANGE` を追加。要素 `a_i = start + i*step` を選ぶので、新しい
  range は `(start + s_val*step, start + e_val*step, step*st)` になる（`s_val` / `e_val` は
  既存の Python スライス規則で丸めた値）。例: `range(10)[::-1]` は `range(9, -1, -1)`、
  `range(0,20,3)[1:3]` は `range(3, 9, 3)`、`range(10)[100:200]` は `range(10, 10)`。
  要素を作らないので巨大 range のスライスも O(1)。step の積が溢れる場合は `OverflowError`。
- `p2c_obj_hash()` に range を追加。等値判定が (長さ, start, step) なのでハッシュも
  それだけから作り、空の range は start/step に関わらず同じハッシュになる
  （辞書のキー・集合の要素として使える）。
- 回帰: `C668–C681`（14 行）。

#### 解析器のバグ修正: 代入位置のスライス

パーサは `a[b:c]` を `p2c_obj_slice(a, b, c, None)` という合成名の呼び出しへ展開するが、
意味解析の `AST_NAME` 解決がこの合成名を「未定義の変数」として拒否していた。そのため
`x = [1,2,3][0:2]` のような**代入右辺のスライスが（range とは無関係に）以前から失敗**して
いた（式文の中では通っていたため気付かれていなかった）。合成名を名前解決の対象外にし、
回帰 `C682–C749`（10 行）で固定した。

#### 解析器のバグ修正: 代入位置のスライス

パーサは  を  という*名前呼び出し*へ展開するが、
意味解析の  解決がこの合成名を「未定義の変数」として拒否していた。
そのため ** のような代入右辺のスライスが（range とは無関係に）
以前から失敗していた**（式文の中では通っていたため気付かれていなかった）。
合成名を名前解決の対象外にし、回帰 （10 行）で固定した。

### Unicode の残り: 幅・精度・大小変換（本セッション）

`%` 書式と `str.rjust/ljust/center/zfill` は **バイト数**で幅を数えていたため、
非 ASCII で 1 文字分ずれていた（`"%5s" % "é"` が 3 スペース、`"あい".center(6)`
が途中で切れる等）。すべて**コードポイント数**で数えるようにした。

| 変更 | 内容 |
|---|---|
| `%s` / `%r` / `%a` | 幅・精度をコードポイント数で適用（`p2c_pf_str_field()` を新設し、snprintf の `%s` を使わない） |
| `%c` | コードポイントの **UTF-8 表現**を出力（以前は 1 バイトを書き出していた）。範囲外は `OverflowError` |
| `rjust` / `ljust` / `center` | 幅判定をコードポイント数にし、終端書き込みを `slen + pad` に修正（`buf[w]` はマルチバイト文字を切り詰めていた） |
| `zfill` | 同じくコードポイント数。**符号は先頭のまま**（`"-12".zfill(5)` は `-0012`） |
| `upper` / `casefold` | 1 コードポイントが複数文字へ展開される特殊ケースを追加（`ß`→`SS`/`ss`、合字 `ﬀ`〜`ﬆ`→`FF`/`FI`/`FL`/`FFI`/`FFL`/`ST`）。`lower` は展開しない（CPython と同じ） |

回帰: `C692–C749`（11 行）。

既知の未対応（据え置き）: `\N{...}`（Unicode 名によるエスケープ。名前表を持たないため未対応）、
`\u0130`.lower() のような多対一の小文字化（`i` + 結合ドット）、`title`/`swapcase` の `ß`→`Ss`。

整数リテラルの対応（完了）: 16 進（0x）・8 進（0o）・2 進（0b）と アンダースコア（1_000）に対応した。レキサーが基数付きリテラルを読み、パーサーが C として妥当な 10 進表記へ正規化する（codegen は p2c_obj_from_int(<text>) を出力するため）。int64 を超える基数付きリテラルは明示的にエラーにする（黙って別の値にならない）。

### ギャップ探索ハーネス（本セッションで新設）

tests/semantic_probe_alpha06.sh は約 50 個の小さなスニペットを CPython と 1 件ずつ比較する。
適合スイートが拾えない差分・クラッシュを洗い出すのが目的。初回実行の結果:

- 33 件一致 / 3 件が意味論差分 / 25 件が失敗（うち大半は下記のコンパイラ・クラッシュ）

#### 重大: コンパイラのヒープ破壊（要修正・再現手順あり）

次の 1 行でトランスパイラが落ちる（free(): double free detected in tcache 2 / corrupted size
vs. prev_size in fastbins）:

    d = {"a": 1, "b": 2}; print(list(d.keys()), list(d.values()))

- print の中で list(...) を 2 つ以上ネストさせると再現する。1 つだけなら落ちない。
- キーを文字列にしても落ちるため、直近の数値リテラル対応とは無関係（既存のバグ）。
- 探索ハーネスを回すまで誰も気付いていなかった（適合スイートはこの形を使っていない）。

#### 意味論差分（3 件）

| スニペット | 期待 | 現状 |
|---|---|---|
| "{1}{0}".format("a", "b") / "{x}".format(x=7) | 位置指定・名前指定の展開 | unhandled exception |
| "{:>5}".format("a") などの文字列への幅指定 | 幅を適用 | 幅が無視される（数値の 05d や .2f は動く） |
| int("ff", 16) / int("0x10", 16) | 基数付き変換 | unhandled exception |
