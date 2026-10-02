# Python 3.13 公式仕様調査メモ

調査日: 2026-08-21

## 一次資料

| 資料 | URL | Alpha0.6への含意 |
|---|---|---|
| What’s New In Python 3.13 | https://docs.python.org/3/whatsnew/3.13.html | Python 3.13の言語・実装・標準ライブラリ変更を区別する。コンパイラの対象は主に言語構文・組込み意味論であり、REPL、free-threading、JITはホストCPython実装の機能である。 |
| PEP 696 | https://peps.python.org/pep-0696/ | 型パラメータの既定値をPython 3.13構文として定義する。型注釈を実行時無視するAlpha0.6では、パラメータリストを構文受理して安全に消去できる候補である。 |
| PEP 667 | https://peps.python.org/pep-0667/ | 最適化スコープの`locals()`は独立スナップショット、`frame.f_locals`はwrite-through proxyという定義になる。フレーム・`exec`・`eval`を持たないfreestandingコアでは完全実装ではなく、明示的な安全フォールバックまたは限定的スナップショットが必要である。 |

## 初期結論

Python 3.13で実行構文そのものとして新規性が大きいのは、PEP 696の型パラメータ既定値である。一方で、3.13までの現代Pythonソースを広く受理するには、既存未対応の`type`文・型パラメータリスト、デコレータ、`raise ... from ...`、`yield from`、`async for`/`async with`、パターン照合の追加形、位置専用・キーワード専用引数、`nonlocal`を除くスコープ構文、追加組込み・データモデルメソッドも重要である。

実装は、生成Cを壊さないことを優先し、(1) 型消去できる構文、(2) 既存ランタイムの明示状態機械へ安全に接続できる構文、(3) C11で実装できる組込み・コンテナ意味論、の順に採用する。CPython内部のGIL、JIT、REPL、frame proxy、完全な`exec`/`eval`はターゲット外とし、明示診断または安全な互換フォールバックを維持する。

## 追加の公式仕様確認

| 資料 | URL | 実装判断 |
|---|---|---|
| Compound statements language reference | https://docs.python.org/3/reference/compound_stmts.html | 文法には`async_for_stmt`、`async_with_stmt`、`async_funcdef`、`match_stmt`、`except*`が含まれる。既存の協調コルーチンは`async def`を持つため、`async for`/`async with`は状態機械の停止・例外・cleanupを横断するため段階的に扱う。`except*`はExceptionGroupの分割規則を必要とし、初期拡張の対象外とする。`match`はAS、sequence、mapping、class、starパターンを段階的に追加できる。 |

## Alpha0.6拡張実装契約

1. **大規模に実装する対象**は、既存C11コード生成に局所的に追加できる`raise ... from ...`、デコレータ適用、`yield from`、位置専用・keyword-only引数の検証、型パラメータと`type`文の構文消去、追加のmatchパターン、`locals()`の明示的スナップショット、C11安全な組込み・コンテナAPIとする。
2. **協調asyncへ拡張する対象**は、既存の状態機械実装が安全に保存・再開・cleanupできる場合の`async for`と`async with`であり、フックが不足する独自awaitable、Task、I/O待機、キャンセル、プリエンプションは対象外とする。
3. **明示的な非対象**は、CPython実装専用のfree-threaded GIL-less実行、JIT、REPLカラー制御、`frame.f_locals` write-through proxy、`eval`/`exec`、ExceptionGroup/`except*`の完全分割意味論である。これらは受理だけして不正なCへ落とすことを禁止する。
4. すべての実装は、対応するCPython差分、直接C不変条件、単一ヘッダー、freestanding、GCC/Clang警告即エラーの品質ゲートへ追加する。

## Python 3.13固定版のsimple statement仕様

| 構文 | 公式仕様 | Alpha0.6実装方針 |
|---|---|---|
| `type Name [type_params] = expression` | Python 3.13の`type`文はsoft keywordであり、遅延評価される`typing.TypeAliasType`を生成する。これは3.12導入構文で、3.13の型パラメータ既定値と組み合わせられる。資料: https://docs.python.org/3.13/reference/simple_stmts.html | freestandingターゲットにはtyping実行時オブジェクトがないため、まず構文と型パラメータ既定値を受理して実行時に安全に消去する。右辺式を評価しないPython型注釈と整合する「型専用alias」互換モードとして明文化する。 |
| `yield from expression` | 同じPython 3.13 simple statementリファレンスのyield規則で定義される委譲yieldであり、完全互換には`send`/`throw`/`close`も関連する。 | 初期段階では既存ジェネレータに対する反復・yield値伝播のみを実装候補とし、`send`/`throw`/`close`を要する完全委譲は安全な明示制限として残す。 |
| `raise exc from cause` | 既存ASTとパーサはcauseを保持済みで、codegenと例外オブジェクトが未接続である。 | 例外オブジェクトにcause文字列表現またはcause参照を追加し、生成Cで評価・保持して例外診断とGCに反映する。 |

## 再調査: 構文拡張候補と優先順位

2026-08-21にPython 3.13固定版の複合文、単純文、式、データモデルを再読した。公式文法は`match`のclosed patternとしてliteral、capture、wildcard、value、group、sequence、mapping、class patternを定義し、successful pattern bindingがguard前に行われることを定める。資料: https://docs.python.org/3.13/reference/compound_stmts.html 。

同資料は`except*`、`async for`、`async with`も定義するが、ExceptionGroupの分割・再結合、または停止・cleanupを横断する状態機械を必要とするため、現行C11ランタイムへの局所追加対象にはしない。generatorの`close()`が3.13で返り値を返せるようになった点は、send/throw/closeとStopIteration valueを含む完全な委譲プロトコルを必要とするため同様に後続対象とする。資料: https://docs.python.org/3.13/reference/expressions.html#generator-iterator-methods 。

今回の優先候補は、既存の`match/case`が持つsubject単一評価、guard、capture、or-patternの構造へ局所追加できる**固定長sequence pattern**、**mapping key pattern**、**as pattern**、および**starred sequence capture**である。これらはiterator停止やCPython固有frameを導入せず、既存list/tuple/str/dictの型・長さ・添字・キーAPIとGCに接続できる。正式な仕様上、matchのname bindingはsuccessful patternの後にguard前で有効となるため、生成Cも同じ順序を守る。

拡張対象外はclass pattern、任意value patternの属性解決、ExceptionGroup/`except*`、async generator、`async for`/`async with`、generator expressionのlazy closure、完全な`yield from`双方向委譲、bytes/bytearray、complexである。これらは受理だけして不正なCを生成せず、明示制限を維持する。
