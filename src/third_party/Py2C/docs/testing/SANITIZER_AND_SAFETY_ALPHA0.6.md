# Python Code to C Alpha0.6 — Sanitizer・実行安全性契約

## 目的

Alpha0.6は、通常のC11警告即エラー構築に加え、Clangの**AddressSanitizer（ASan）**と**UndefinedBehaviorSanitizer（UBSan）**による実行時検査を正式な品質ゲートにする。ASanはheap/stack/globalの範囲外アクセス、use-after-free、double-freeなどを検出し、UBSanは不正なシフト、null・misaligned pointer、符号付き整数overflowなどの未定義動作を実行時に検出する。[1] [2]

| 品質ゲート | 実行コマンド | 対象 | 失敗条件 |
|---|---|---|---|
| 通常C11 | `make CC=clang full-build && make CC=clang test` | Hosted、freestanding、単一ヘッダー、差分回帰 | 警告、構築失敗、出力差分 |
| GCC互換 | `make CC=gcc full-build && make CC=gcc test` | 非Clang依存性 | 警告、構築失敗、出力差分 |
| Sanitizer | `make CC=clang test-sanitizers` | compiler本体、parser、GC lifecycle/allocator rollback、383件CPython差分、生成C runtime | ASan/UBSan診断、出力差分 |
| GC LeakSanitizer | `make CC=clang test-gc-leaks` | 複数runtime epoch、registry reset、root解除、cycle回収、shutdown | runtime所有heapのleak、ASan/UBSan診断 |

`test-sanitizers`は`-fsanitize=address,undefined`、`-fno-omit-frame-pointer`、デバッグ情報を付与してcompiler本体を別の`build/sanitize`・`obj/sanitize`へ構築する。生成Cとその同梱runtimeも同じsanitizerで再コンパイルするため、変換器だけでなく生成コードの実行経路も検査対象となる。ASanとUBSanはテスト専用であり、freestanding配布物または本番の自作OSバイナリへ導入しない。[1] [2]

## 修正済み: starredアンパック代入のuse-after-free

`a, *mid, z = seq`の解析では、従来`TOK_STAR`を指す`P2C_Token*`を保存した後に`NEXT(p)`でlexerを進め、解放済みtokenからline/columnを読む経路があった。Alpha0.6では`NEXT(p)`より前に`star_line`と`star_col`を値としてコピーし、AST node生成時はその値だけを使用する。

`tests/parser_starred_unpack_asan_alpha06.py`はlistとtupleのstarredアンパックを含み、`test-parser-sanitizers`が同入力の変換、生成Cのsanitizer実行、CPython互換出力を検証する。最新のsanitizerゲートでは、parser回帰、generator式関数引数回帰、builtin key回帰、例外cause回帰、async with回帰、set comprehension回帰、decoratorのcallable adapter・GC root・async/class再束縛回帰、**runtime shutdown/reinit・静的module slot reset・constructor rollback**、およびC01–C383の383件差分がASan/UBSan診断なしで通過した。

## リーク検査の扱い

CLIはプロセス単位でarena/GC管理された解析・AST資源を終了時まで保持する設計である。これは短命コンパイラでは許容しうる設計上の選択だが、長時間常駐するGUI、HTTPサービス、または埋込みホストのリーク安全性を保証するものではない。そのため既定sanitizerゲートは`ASAN_OPTIONS=detect_leaks=0`とし、use-after-free・範囲外アクセス・未定義動作を停止条件にする。LeakSanitizerはLinuxでASanと併用できる。Alpha0.6では通常の短命CLI検証とは分離し、`test-gc-leaks`で複数runtime epochを実行してから`p2c_runtime_shutdown()`を呼び、runtime所有object・module registry・class registry・async queueが残らないことを検証する。[1]

## 外部コマンド実行の境界

`make run`および一部のGUI/開発補助経路は、生成Cのコンパイルまたは実行のためにホストのCコンパイラを起動する。この経路は**信頼されたローカル開発入力**を前提とする。コンパイラ入力、出力パス、コンパイラ選択、追加フラグを不特定利用者が指定できるサービスへ直接公開してはならない。サーバ用途では、allowlistされたcompiler path、固定作業ディレクトリ、引数配列によるprocess起動、権限分離、資源上限を備えた別の実行アダプタを用いる。

## CI

`.github/workflows/quality.yml`はpush、pull request、手動実行で以下を走らせる。

| Job | 実行内容 |
|---|---|
| Native quality（Clang/GCC） | `full-build`と`test` |
| AddressSanitizer and UBSan | `make CC=clang test-sanitizers` |
| GC lifecycle LeakSanitizer | `make CC=clang test-gc-leaks` |

## 参考資料

[1]: https://clang.llvm.org/docs/AddressSanitizer.html "Clang AddressSanitizer Documentation"
[2]: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html "Clang UndefinedBehaviorSanitizer Documentation"
