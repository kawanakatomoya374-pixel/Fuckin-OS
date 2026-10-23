# Python Code to C Alpha1.0 Documentation

Alpha1.0の資料は、目的別に以下のカテゴリへ整理しています。

| カテゴリ | 内容 |
|---|---|
| [spec](spec/) | 対応構文、Python 3.13互換性、構文拡張、移植性仕様 |
| [build](build/) | 起動、ビルド、単一ヘッダー、freestanding、async/bare-metal契約 |
| [testing](testing/) | コンフォーマンス、ファジング、sanitizer、テストレポート |
| [runtime](runtime/) | ランタイム、コンテナ、GC、API拡張 |
| [review](review/) | 監査、品質基線、厳格レビュー、コード方針 |
| [release](release/) | Alpha1.0リリースノート |
| [research](research/) | Python公式仕様と設計調査 |

## 直近の構文拡張

`async with`の状態機械、class内protocol methodのawait、set comprehensionの通常生成とstrict ISO C11診断は、[async with・set comprehension拡張仕様](spec/ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA1.0.md)にまとめています。module-level function/class decoratorの評価順、callable再束縛、GC root、期待診断は、[decorator拡張仕様](spec/DECORATORS_ALPHA1.0.md)にまとめています。常駐hostのshutdown/reinit、registry reset、allocator rollback、LeakSanitizer境界は、[GC hardening監査](review/GC_HARDENING_AUDIT_ALPHA1.0.md)を参照してください。CPython差分IDは[コンフォーマンス台帳](testing/CONFORMANCE_TEST_MATRIX_ALPHA1.0.md)を参照してください。
