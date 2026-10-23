# Python Code to C Alpha1.0 — GC Hardening Audit

**Author:** Manus AI  
**Date:** 2026-08-22  
**Scope:** 長時間稼働ホスト、自作OS組込み、runtime shutdown/reinit、GC root、allocator失敗、ASan/UBSan/LeakSanitizer境界。

## 1. 監査結論

Alpha1.0のGCは、container、closure/cell、generator/coroutine、exception cause、function environment、instance/class/module attribute mapを到達性グラフとして走査する。明示root、内部registry、pin counter、保守的stack scanを組み合わせる設計は維持する。監査では、shutdown/reinit後にmodule/class registryが解放済み`P2C_Object*`を保持するuse-after-freeと、constructor後段allocation失敗で新規objectがGC追跡リストに残り得る問題を再現し、修正した。

| 監査項目 | 結果 | 根拠 |
|---|---|---|
| object graph traversal | 継続監査 | list/dict/set/tuple/function/cell/iterator/exception/instance/class/moduleのchildを`gc_traverse`で列挙 |
| explicit root | 登録重複を抑止 | slot addressを重複検査し、root capacity超過はplatform abort |
| conservative stack scan | Hosted Linuxで有効。**走査範囲は「現在のフレームから宣言上端まで」** | pthread stack boundary（または`p2c_gc_init()`の上端ヒント／`p2c_gc_set_stack_bounds()`）を受け、使用中の範囲だけをword単位で照合する。区間全体を走査しないため、しきい値を小さくしても「収集回数×スタック長」に劣化しない。`p2c_gc_last_stack_words()`で語数を確認できる |
| shutdown/reinit | **修正済み** | ASanで再現したregistry経由UAFに対しepoch resetを実装し、3 epochで再検証 |
| allocation rollback | **修正済み** | dict/set/tuple/string、class/instance/module/function/closure、文字列反復の後段確保失敗で新規objectをGC listから除去 |
| LeakSanitizer | **分離gateを実装** | 3 epochの正常shutdown後、runtime所有heapにleak報告なし |
| 共有ヒープ（プラットフォーム） | **修正済み** | 変換器コア（文字列ビルダ等）とランタイムが別アロケータを同じ領域に対して使うと、片方の確保が他方のobjectを上書きする。共通層の`p2c_heap_*`へ一本化し、`make test-baremetal-exceptions`で回帰を防止 |

## 2. 再現済み不具合 GCH-001

`tests/test_gc_runtime_reinit.c`は、runtime初期化、class作成、shutdown、再初期化、GC実行を行う。この再現器を`-fsanitize=address,undefined`で実行すると、`gc_mark()`が既にshutdownで解放したobjectを読んだとしてASanが停止する。

> 原因は、`p2c_runtime_shutdown()`が`g_gc_all`のobjectを解放しても、`g_module_registry`と`g_class_registry_objs`をリセット・解放しないことにある。次回`p2c_gc_collect()`がこれらのregistryをrootとしてmarkし、解放済みobjectを辿る。

修正後の受入条件は、同一プロセスで複数回のinit/shutdownを繰り返してもASan/UBSan診断がなく、module/class registry、async queue、pygame static class slot、root tableが新しいruntime epochへ持ち越されないことである。`make CC=clang test-gc-leaks`は同じ3 epochをLeakSanitizerで検証する。

## 3. 強化方針

| ID | 対策 | 受入条件 |
|---|---|---|
| GCH-001 | shutdownでmodule registry metadata、class registry、root table、GC統計、stack boundary、active exceptionを完全reset | reinit regressionがASan/UBSanで成功 |
| GCH-002 | runtime epoch/stateを明示化し、shutdown済みruntimeへのGC/root API呼出しを安全なno-opまたは診断へ限定 | lifecycle regressionで再初期化後の統計とroot数が初期状態 |
| GCH-003 | root登録解除、pin/unpin、cycle回収、generator await graphを繰返しstress | collection回数、object数、root数が契約どおり推移 |
| GCH-004 | allocator失敗時のobject constructor・文字列反復・attribute mapをASan/UBSanで検査 | `test-gc-allocation-failure`が新規object数不変と安全な失敗戻りを確認 |
| GCH-005 | LeakSanitizer専用の短命host gateを追加 | 正常shutdown後にruntime所有heapのleak報告なし |

## 4. 保証の境界

この監査は「完全性」を数学的に主張しない。保守的stack scanは偽陽性を許容するため、到達不能objectの回収を遅らせることがある。また、GCが認識できないOS固有キュー、DMA buffer、割込みハンドラ、foreign threadのメモリに保存した`P2C_Object*`は自動検出できない。組込みホストはそのような保持を明示root slotまたは`p2c_obj_incref()`/`p2c_obj_decref()`で契約化する必要がある。

## 5. 共有ヒープの注入とライフサイクル（Alpha1.0 追補）

§1の「共有ヒープ」修正の後続として、ヒープの実体を「OSの1つ」に固定するための
差し替え点と、解除順序の境界を整理した。

| ID | 対策 | 受入条件 |
|---|---|---|
| GCH-006 | アロケータ注入API（`p2c_platform_set_allocator()`＝カーネルの1ヒープ、`p2c_set_default_allocator()`＝`P2C_Allocator`）を追加 | 注入が変換器コアとランタイムの両方へ効き、`make test-allocator-injection`が共有ヒープ経由の確保と解放を確認する |
| GCH-007 | プラットフォーム解除後に残ったmagic付きブロックを、ペイロードではなくブロック先頭から解放する。magic判定はプラットフォームブロックが1つでも生きている間だけ行い、純粋なlibcブロックの手前を読まない | 停止順序（ランタイム停止→プラットフォーム解除）でASan/UBSan診断なし |
| GCH-008 | 変換器コアの静的フォールバックをプローブ判定にし、サイズ変更（`P2C_COMPILER_FALLBACK_HEAP_SIZE`）と無効化（`=0`）を可能にする | 注入が有効な限り静的バッファを使わない（`p2c_core_static_allocator_active() == false`）。無効化時は未注入構成で`P2C_ERR_NOMEM`と注入方法を返す |
| GCH-009 | NO_STDLIB のランタイム同梱スタブ（`malloc/calloc/realloc/free`）を共有ヒープ層（`p2c_heap_*`）の別名にし、カーネルアロケータを注入した構成では raw malloc も変換器・ランタイムと**同じ1つのヒープ**を使う。ブロックヘッダ（サイズ + magic）を全提供元で共通化し、線形ヒープ由来の magic を追加。線形ヒープの `realloc` が旧ブロック長を越えてコピーし得た経路を除去 | `make test-heap-unification` が「別ヒープを確保しない（`p2c_runtime_heap_used() == 0`）」「raw と `p2c_heap_*` の相互解放」「線形 `realloc` の旧サイズコピー」「ヒープ未設定時の NULL」を確認する |

> 「注入が効いているか」は`p2c_heap_uses_platform()`（ヒープ実体）と
> `p2c_core_static_allocator_active()`（変換器コアの実体）の2つで観測できる。
> 常駐サービス構成では後者が`false`であることを起動時に検証するのが望ましい。

## 6. 式評価中の一時値と例外脱出（GCH-010）

§1〜§5は「GCが保持グラフを見落とす」経路を扱ったが、生成Cには
**保守的スタックスキャンから見えないTLS一時値**がもう1種類あった。

生成Cは二項演算を

```
(p2c_binop_begin(left), p2c_binop_finish(p2c_obj_add, right))
```

の形で出す。`left`は`p2c_binop_finish()`が取り出すまでTLSのbinopスタック
（`g_binop_stack`）にしか存在せず、`right`の評価中に自動収集が走ると`left`が
回収され、解放済みポインタが演算関数へ渡っていた（静かなuse-after-free）。
処理中の例外（`p2c_active_exception`）も同じくTLSにしか無い期間がある。

さらに、右オペランドの評価中に例外が脱出する（`longjmp`する）と、生成Cの`try`に
保存/復元が無いため深さだけが増えたままになった。反復すると65回目で
`p2c_binop_begin()`が「binary expression nesting limit exceeded」(RuntimeError)を
誤発火し、f-stringは33回目で同じくネスト上限になり、ビルダのネイティブ確保が
解放されずリークしていた（`RuntimeError`は`except ValueError`を素通りするため、
利用側からは原因不明の異常終了に見えた）。

| ID | 対策 | 受入条件 |
|---|---|---|
| GCH-010 | (A) マークフェーズでTLS一時値（binopスタックの全段、`p2c_active_exception`）を明示的にルート化する。(B) 生成Cの`try`が開始時の`p2c_binop_depth()`/`p2c_fstr_depth()`を保存し、例外ハンドラ到達時と未処理の再送出直前に`p2c_binop_rewind()`/`p2c_fstr_rewind()`で戻す。`p2c_runtime_shutdown()`は両方を空にする | `make test-gc-temp-roots`が「式の途中の収集で左オペランドが壊れない」「処理中の例外が回収されない」「200回例外が脱出しても深さが漏れず、その後の式が正しい」「停止後に深さ0」を確認する（LeakSanitizer付きのリンクを含む）。`tests/audit_regression.sh`のGCH-010ケースがCPythonと同一出力になる |

観測・契約API:

| 関数 | 用途 |
|---|---|
| `size_t p2c_binop_depth(void)` | binopスタックの現在の深さ。`try`開始時に保存する |
| `void p2c_binop_rewind(size_t depth)` | 保存した深さまで戻す（冪等。容量を超える値は全消去） |
| `size_t p2c_fstr_depth(void)` | f-stringビルダスタックの現在の深さ |
| `void p2c_fstr_rewind(size_t depth)` | 同上。溢れたビルダはここで解放される |
| `size_t p2c_gc_last_temp_roots(void)` | 直近の収集でルート化したTLS一時値の個数（0なら式の途中で収集が走っていない。組込みホストの診断用） |

> カーネルやアプリが`P2C_LONGJMP`を直接使って自前で脱出する場合は、
> 例外フレームへ到達した時点で保存した深さへ戻す契約が必要になる。
> 生成C（`--embed-entry`を含む）はこの契約を自分で守る。

検証は両方向の欠陥注入で行った。マークフェーズの呼び出し
（`g_gc_temp_roots = gc_mark_tls_temporaries();`）を外すと
`make test-gc-temp-roots` は失敗し（`p2c_gc_last_temp_roots()`が0、左オペランドの
検算も不一致）、`p2c_binop_rewind()`/`p2c_fstr_rewind()`を無効化すると同テストと
`tests/gc_temp_roots_alpha10.py`の生成C実行が
`RuntimeError: f-string nesting limit exceeded`で失敗する。
