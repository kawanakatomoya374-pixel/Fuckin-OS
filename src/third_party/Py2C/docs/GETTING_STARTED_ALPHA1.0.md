# Python Code to C Alpha1.0 — はじめの一歩

このドキュメントは、**Python は書けるが C やコンパイラに詳しくない人**が、
このツールを最短で使えるようにするための入口です。細かい仕様は
`docs/spec/FEATURE_REFERENCE_ALPHA1.0.md`、設計の背景は `docs/review/` を参照してください。

## 1. これは何をするツールか

Python のコードを読み、**同じ意味で動く C11 のコード**へ変換します。生成された C は
次のような場面で使えます。

- Python で書いた処理を、C しか動かない環境（組込み・自作OS・小さな実行環境）へ持っていく
- 「Python のロジックを C で配布したい」ときに、手で書き直す手間を減らす
- 生成された C を読んで、Python がどう C に対応するかを学ぶ

変換の流れは次の 4 段階です。`-v` を付けると各段階の時間と統計が見えます。

```
hello.py --[字句解析]--> トークン列 --[構文解析]--> AST
         --[意味解析]--> 名前解決済み AST --[コード生成]--> hello.c
```

## 2. まず動かす（3 コマンド）

```sh
make                    # ビルド（build/python-code-to-c と build/py2c ができます）
echo 'print("hello")' > hello.py
./build/py2c help       # 使い方の表示（Py2C help でも同じ）
./build/py2c run hello.py
```

`py2c run` は **変換 → C コンパイル → 実行** をまとめて行うので、最初はこれだけ覚えれば十分です。
`hello` と表示されれば成功です。

## 3. C のコードだけが欲しいとき

```sh
./build/py2c hello.py -o hello.c
```

生成された `hello.c` を自分でビルドする場合は、同梱のランタイムを一緒にコンパイルします
（この手順は `py2c help` にも出ています）。

```sh
cc -I./include hello.c \
   src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c \
   src/platform/python_code_to_c_platform.c \
   src/platform/python_code_to_c_platform_hosted.c \
   src/modules/python_code_to_c_pygame.c -lm -o hello
./hello
```

## 4. よく使うオプション

| オプション | 何をするか | いつ使うか |
|---|---|---|
| `-o out.c` | 出力ファイル名 | 生成 C を残したいとき |
| `-v` | 各段階の所要時間と統計 | 変換が遅い/何をしているか知りたいとき |
| `--comments` | 生成 C に元の Python 行をコメントで残す | 生成 C を読んで理解したいとき |
| `--c11` | GNU 拡張が必要な構文を拒否し ISO C11 に限定 | 厳密な C11 環境へ持っていくとき |
| `--fallback` | 未対応構文を「実行時エラーになるコード」へ置換して変換続行 | まず全体を通して動かしたいとき |
| `--embed-entry NAME` | `int main()` の代わりに `P2C_Object *NAME(void)` を生成 | 自作OSのカーネルから呼ぶとき |

## 5. つまずいたときの切り分け

1. **`Parse error` / `Code generation error` が出た**
   - その構文はまだ未対応です。メッセージの行・列が原因箇所を示します。
   - `./build/py2c --supported` で対応範囲を確認し、必要なら `--fallback` を付けます
     （変換は続き、その部分は実行時に `NotImplementedError` として検出できます）。
2. **生成 C がコンパイルできない**
   - 変換時にエラーが出ていないか確認してください（エラーがあると C は生成されません）。
   - 生成 C は `-Wall -Wextra -Werror` 相当の厳しい基準で検証されています。まず変換メッセージを
     解消するのが早道です。
3. **実行結果が Python と違う**
   - `./build/py2c --dump-ast file.py` で、ツールが構文をどう解釈したかを確認できます。
   - 未対応/差異の一覧は `docs/spec/FEATURE_REFERENCE_ALPHA1.0.md` の「既知の制限」にあります。
4. **ツール自体が怪しい**
   - `./build/py2c --self-test` が、同梱の例を変換・実行して一括確認します。

## 6. 知っておくと役立つ 3 つの仕様

- **GC は保守的（conservative）**: スタック上に置いたポインタも「生きている」とみなします。
  そのため C 側でポインタを隠しすぎると回収されません。逆に、配列などスタックから見えない場所へ
  オブジェクトを置くときは、ランタイムが用意したアンカー/ルート登録を使ってください。
- **例外は setjmp/longjmp で実装**: Python の `try`/`except`/`finally` は C の非局所脱出で実現します。
  このため、`try` をまたいで変更した自動変数は `volatile` を付けてください（生成コードは
  自動でそうしています）。
- **クロージャは値を捕獲**: 現在の実装はラムダ/入れ子関数が参照する外側の変数を**値として**
  取り込みます。Python の遅延束縛（`for i in ...: fns.append(lambda: i)` が最後の `i` を返す）
  とは異なり、**その時点の値**を返します。ループ変数を固定したい場合は
  `lambda i=i: ...` のように既定引数で束縛してください。

## 7. 開発者向け（品質の基準）

- 既定ビルドは `-Werror` 付きの非常に厳しい警告基準（`Makefile` の `WARN_CFLAGS`）で通ります。
- 生成 C も成果物として同じ「実バグを捕まえる」基準（`GENERATED_CFLAGS`）で検査されます。
- `make test` が、単一ヘッダー/組込み/自作OS/サニタイザ/CPython 差分（800 件超）を一括実行します。
- 組込み向けのスタック予算は `make test-stack-budget` が検証します（コア 1 フレーム 4096 バイト以内）。
