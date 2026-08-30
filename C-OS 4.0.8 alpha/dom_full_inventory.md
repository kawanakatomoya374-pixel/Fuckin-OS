# C-OS QuickJS-NG / NetSurf DOM 実装台帳

更新日: 2026-08-29

本台帳は、C-OS上のQuickJS-NG DOMブリッジについて、実装済み範囲、QEMUで確認した範囲、残存するWHATWG仕様差分を分離して記録する。**現時点では完全なWHATWG DOM準拠ではなく、主要な自己テストを通過した実用サブセットである。**

## QEMU確認済み

| 領域 | 自己テスト | 結果 |
|---|---|---|
| Range | `quickjs_range_selftest.html` | `RANGE_PASS` |
| Selection | `quickjs_selection_selftest.html` | `SELECTION_PASS` |
| Shadow DOM / slot | `quickjs_shadow_slot_selftest.html` | `SHADOW_SLOT_PASS` |
| CharacterData / MutationObserver | `quickjs_mutation_characterdata_selftest.html` | `MUTATION_CHARACTERDATA_PASS` |
| 複数MutationObserver | `quickjs_mutation_multiple_observers_selftest.html` | 既存確認済み |
| MutationObserver options | `quickjs_mutation_observer_options_selftest.html` | 既存確認済み |
| dataset | `quickjs_element_dataset_selftest.html` | `DATASET_PASS` |
| Comment | `quickjs_create_comment_selftest.html` | `CREATE_COMMENT_PASS` |
| DOMParser / XMLSerializer | `quickjs_domparser_selftest.html`, `quickjs_serializer_selftest.html` | 既存確認済み |
| DOMException | `quickjs_domexception_selftest.html` | `DOMEXCEPTION_PASS` |

上記の各QEMU実行では、対応するPASSログに加えて、ページフォルト、未処理例外、QuickJS参照カウントassert、filesystem read failedを検出していない。QEMUはタイムアウトで停止するため、`qemu_exit=124`は自己テスト完了後のランナー終了を表す。

## 実装済みまたは接続済みの主要機能

| API群 | 現状 |
|---|---|
| Node / Element | libdomノードラッパー、子ノード操作、属性、`textContent`、`innerHTML`、`outerHTML`、`parentNode`、`childNodes`、`cloneNode`、`normalize`、`getRootNode`、`isConnected`、比較系の一部 |
| CharacterData | Text / Comment の `length`、`substringData`、`appendData`、`insertData`、`deleteData`、`replaceData`。変更前のネイティブ旧値スナップショットをCバッファへ複製 |
| MutationObserver | 複数observer、独立キュー、`takeRecords`、microtask通知、`subtree`、`attributeFilter`、`attributeOldValue`、`characterDataOldValue` |
| Element補助 | `dataset` Proxy、camelCase変換、data属性とのlive反映、`classList`、`style`、基本の反映属性 |
| Range / Selection | 生成、境界設定、collapse、selectNode、selectNodeContents、文字列化、削除、挿入、複製の主要経路。高度な境界アルゴリズムは未達 |
| Shadow DOM | open / closed root、`slot`、`name`、`assignedNodes`、`assignedSlot`、append時のslot割当、slotchange通知、remove時の割当解除 |
| DOMParser / XMLSerializer | MIME型検証、Web IDL文字列変換、HTML系の既存独立モデル、Comment serialization。完全なXML parser/document modelではない |
| DOMException | `instanceof`、名前、メッセージ、主要legacy code（`TypeMismatchError`=17等） |

## 残存する仕様差分

| 領域 | 残存課題 | 優先度 |
|---|---|---|
| Shadow DOM | default slotの厳密な割当、slot属性・slot名変更時の完全再配布、複数slotの順序、fallback content、closed rootの内部境界、composed event retargeting | 高 |
| Range | DOM tree boundary point、部分選択、複数ノード削除・抽出・clone、各種例外（HierarchyRequestError等）、live range更新 | 高 |
| Selection | 複数range、anchor/focus方向、extend・modify、document selectionとの完全同期 | 高 |
| DOMParser | 独立Documentモデル、XML well-formedness、名前空間、doctype、parsererror、CDATA/PI、HTML fragment context | 高 |
| Web IDL | 全APIのbrand check、nullable/sequence/dictionary変換、getter/setterのdescriptor、例外型と引数arityの統一 | 高 |
| Node / Element | `compareDocumentPosition`等の定数・順序厳密性、イベントパス、document adoption/import、template/content、forms/selectorsの網羅性 | 中 |
| MutationObserver | libdom内部の全mutation sourceとの完全統合、coalescing・record ordering・transient registered observer、attribute namespace | 中 |

## 作業上の注意

自己テストPASSは、対象テストが検証する経路の成功を意味する。**WHATWG DOM全件の完全準拠を意味しない。** 次段階では、上表の高優先度項目を一つずつ仕様テスト化し、QEMUで再現可能な最小ケースとして追加する。
