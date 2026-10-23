# Python Code to C Alpha1.0 — dict・set差分ファジング

## 1. 目的

このファジングは、`dict`と`set`に対する**決定的な操作列**を生成し、CPythonの出力とPython Code to Cが生成・実行するCプログラムの出力を比較します。通常の回帰ケースが個別の意味論契約を固定するのに対し、ファジングは複数の更新、削除、順序、集合演算、包含関係が連続した状態遷移で壊れないことを検証します。

> 生成器は固定seedを使用します。同一のseed、操作数、コンパイラ指定を渡せば、同じPython入力、生成C、比較出力を再現できます。外部ネットワーク、時刻、乱数源、ホスト固有のhash順序には依存しません。

## 2. 対象の操作

| 構造 | 生成する操作 | 確認する不変条件 |
|---|---|---|
| dict | 添字代入、`setdefault`、既定値付き`pop`、pair iterableによる`update`、`dict.fromkeys` | 重複キーの後勝ち、挿入順、共有value、キー削除後の状態 |
| set | `add`、`discard`、`update`、`intersection_update`、`difference_update`、`symmetric_difference_update` | 重複排除、破壊的更新、空集合・部分集合・上位集合の意味論 |
| 観測 | `list(d.items())`、`sorted(s)`、`issubset`、`issuperset`、`isdisjoint` | CPythonと生成Cの出力完全一致 |

ファズ操作は有効なPython入力だけを生成します。例外型、引数数、境界、共有mutable値、空集合エラーなどの明示的な失敗モードは、CPython差分コーパスC275–C282に固定します。

## 3. 標準実行

通常の品質ゲートには3 seed、各48操作のファジングが含まれます。

```sh
make test-container-fuzz
make test
```

明示的にClangを使う場合は次のとおりです。

```sh
make CC=clang test-container-fuzz
```

このターゲットは、各seedについて次の順序で処理します。

1. `tests/generate_container_fuzz.py`がPython入力を生成します。
2. CPythonが基準出力を生成します。
3. Hosted CLIが入力をCへ変換します。
4. 指定Cコンパイラがランタイム・プラットフォーム中核とともに生成Cを警告即エラーでビルドします。
5. 生成プログラムの出力をCPython出力と完全比較します。

## 4. 再現可能な強化実行

seed集合と操作数はmake変数として上書きできます。通常の変更前後比較、手元での深掘り、CIでの拡張検証に使います。

```sh
make CC=clang \
  P2C_FUZZ_CASES=64 \
  P2C_FUZZ_SEEDS='1 7 42 99 31337 65537 104729 2147483647' \
  test-container-fuzz
```

| 変数 | 既定値 | 意味 |
|---|---:|---|
| `P2C_FUZZ_CASES` | `48` | 1 seedあたりの状態更新数。1から1000まで指定可能 |
| `P2C_FUZZ_SEEDS` | `12648430 24237 17412` | 空白区切りの10進数または`0x`付き整数seed |
| `CC` | makeの既定Cコンパイラ | 生成CをビルドするCコンパイラ |

## 5. 失敗時の取り扱い

比較、変換、またはCコンパイルが失敗した場合、ランナーは同一seedに対して**最小失敗prefix**を二分探索します。再現入力、CPython出力、生成C、生成プログラム出力、diffは次の配下に保存されます。

```text
build/tests/container_fuzz/repro-seed-<seed>-cases-<minimal-cases>/
```

標準エラーには、そのまま実行できる再現コマンドが表示されます。

```sh
CC=clang P2C_FUZZ_SEEDS=<seed> P2C_FUZZ_CASES=<minimal-cases> \
  sh tests/container_fuzz_regression.sh
```

再現後は、失敗した最小操作列から意図した意味論を切り出し、`tests/`に決定的なCPython差分ケースとして追加します。修正後も元seedを`P2C_FUZZ_SEEDS`に残し、通常の固定回帰とファジングの両方で確認します。

## 6. 設計上の境界

このファジングは、dict・setの内部順序やC実装のバケット配置を直接の仕様にはしません。setの観測値は`sorted()`で正規化し、Pythonが順序を保証するdict項目列だけを`list(d.items())`で観測します。これにより、ハッシュ実装の変更やCコンパイラの差異ではなく、Pythonレベルの意味論差分だけを失敗として扱います。

また、ファジングは回帰試験を置き換えません。固定コーパスは例外メッセージ、制御移譲、型境界、単一ヘッダー、freestanding、GC、GUIを含む明示的な契約を提供し、ファジングは連続したコンテナ状態遷移の探索を補完します。
