# C99 / TinyCC ビルドと ELF 生成 (Alpha0.6)

このドキュメントは、Alpha0.6 で追加した **C99（TinyCCを含む）ビルド経路** と
**ELF 生成**（完全版ELF / HobbyOS向けELF）の使い方と設計をまとめたものです。
既定のC11＋厳格警告ビルド（`WARN_CFLAGS`）は変更していません。

## 1. なぜC99でもビルドできるようにしたか

組み込み向けのツールチェーンには、C11の一部（`_Thread_local`、`_Static_assert`、
`_Generic` など）やGCC固有の組み込み（`__builtin_setjmp` 等）を持たないものが
あります。TinyCC (tcc) はその代表で、小さく高速にビルドできるため、自作OSの
ビルド環境やCIの軽量ゲートとして使えます。そこで:

- コアは **C99で書ける範囲** に保ち、C11が必要な箇所は移植マクロで吸収する
  （`include/common/python_code_to_c_common.h` の `P2C_THREAD_LOCAL` など）。
- `__builtin_setjmp` / `__builtin_longjmp` は GCC/Clang 限定とし、tcc では
  libc の `setjmp`/`longjmp`（ホスト構成）またはカーネル実装
  （`PYTHON_CODE_TO_C_NO_LIBC_STUBS`）を使う。
- `no_sanitize` などの属性は GCC/Clang のときだけ付ける。

## 2. C99ビルド（GCC）

```sh
make c99          # build/c99/python-code-to-c（-std=c99 -pedantic -Wall -Wextra -Werror）
make test-c99     # C99でビルドした変換器でCPython差分コーパス（549アサーション）を実行
```

`make c99` は **コンパイラ本体を `-std=c99 -pedantic` でビルド** します。
これにより「C99として妥当である」ことを機械的に検証できます（拡張構文を
使ってしまうと `-pedantic -Werror` で落ちます）。

生成C側は、内包表記がGNU statement expressionを使うため `-pedantic` を付けず
`-std=c99` でコンパイルします（`--c11` は変換時にその構文を拒否します）。
`test-c99` はこの条件で全ケースを変換・コンパイル・実行し、CPythonと一致する
ことを確認します。

## 3. TinyCCビルド

```sh
make tcc          # CC=tcc でCLIをビルド（build/tcc/python-code-to-c）
make test-tcc     # tccで変換器と生成Cの両方をビルドして差分コーパスを実行
```

`tcc` は `-Wconversion` などのGCC固有の警告を解釈しないため、C99構成では
`-Wall -Werror` のような共通部分だけを有効化します（C11の厳格基準は不変です）。
tccが見つからない場合は `check-tcc` が理由を表示して停止します
（`TCC=/path/to/tcc` で場所を指定できます）。

## 4. ELF生成

### 4-1. 完全版ELF（`make elf`）

```sh
make elf
# build/elf/python-code-to-c.elf        … 静的リンクされた自己完結ELF（主成果物）
# build/elf/python-code-to-c            … 同じ内容の互換用コピー（拡張子なし）
# build/elf/python-code-to-c.elf.txt    … file/readelf のマニフェスト
```

ホスト機能（CLI・ファイル入出力・pygameヘッドレス等）をすべて含む実行ファイルを
**静的リンク** で作ります。動的ローダに依存しないため、同じELFを別のLinux環境へ
そのまま持ち込めます。生成後は次を自動検査します。

- `readelf -l` に `INTERP` セグメントが無い（＝動的リンクではない）
- ELFヘッダが `EXEC`（位置独立実行形式ではない）
- 実際に実行して `--supported` が1行目を出力する

### 4-2. HobbyOS向けELF（`make hobbyos elf` / `make hobbyos-elf`）

```sh
make hobbyos elf        # または make hobbyos-elf
# build/hobbyos/python-code-to-c-hobbyos.elf      … libc非依存の自己完結ELF
# build/hobbyos/python-code-to-c-hobbyos.elf.txt  … マニフェスト
# build/hobbyos/program.c                         … --embed-entry で変換したプログラム
```

HobbyOSにそのままロードできるELFです。構成は次のとおりです。

| 項目 | 内容 |
| --- | --- |
| エントリ | `p2c_hobbyos_entry()`（カーネルがロード後に呼ぶ） |
| 変換対象 | `examples/embed/embed_boot.py` を `--embed-entry p2c_hobbyos_program` で変換（`HOBBYOS_ELF_PROGRAM` で変更可） |
| リンク | `-nostdlib -static`、`templates/hobby_os/hobbyos.ld`、`-lgcc` |
| ロードアドレス | 既定 `0x400000`（`-DHOBBYOS_LOAD_ADDR=...` で変更） |
| セグメント | `.text`(R+X) / `.rodata`(R) / `.data`+`.bss`(R+W) の3つ（W^X） |
| 出力 | `p2c_hobbyos_output(data,len)` をカーネルが設定（NULLなら出力なし）。直近の出力は `p2c_hobbyos_console_text()` |

生成後は次を自動検査します。

- `INTERP` セグメントが無い（動的リンクではない）
- **未定義シンボルが無い**（`nm -u`）— ランタイム同梱の最小libcと
  カーネルフックだけで完結している
- マニフェストに `readelf -h` / セクション一覧を記録

### 4-3. 参照libc（`templates/hobby_os/embed/hobby_os_libc.c`）

`NO_STDLIB` のランタイムは、次の関数を **カーネル側が提供する契約** として
参照します（`include/common/python_code_to_c_common.h` に宣言）。

- libm: `floor ceil trunc fabs fmod pow sqrt cbrt hypot copysign ldexp sin cos tan
  asin acos atan atan2 exp expm1 log log2 log10 log1p erf erfc tgamma lgamma`
- libc: `snprintf vsnprintf strtoll strtod`

実カーネルは自前の実装へ置き換えるのが本筋ですが、そのまま組み込めるELFと
libcを持たない小さいターゲットのために、参照実装を同梱しています。精度方針は
「組込み用途で実用的な範囲」で、最終ビットまでは保証しません。境界（±inf、NaN、
0、負値）はPythonの期待に合わせます。

参照実装はホストのlibm/libcと比較して検証します:

```sh
make test-hobbyos-libc   # 全関数をlibmと比較（シンボルはstub_前置で衝突回避）
```

## 5. 単一ヘッダー（`include/python_code_to_c_single.h`）のC99/TinyCC検証

単一ヘッダー（コア＋ランタイムを1ファイルへ結合したもの）も、C99とTinyCCで
完全にビルドできることを検証しています。

```sh
make single-header                 # 再生成
make test-single-header            # GCC（既定C11）でビルドして実行
make test-single-header-c11        # GCC -std=c11 -pedantic-errors
make test-single-header-c99        # GCC -std=c99 -pedantic-errors -Wall -Wextra -Werror
make test-single-header-freestanding   # GCC NO_STDLIB（hosted libc参照が無いことを確認）
make test-single-header-tcc        # TinyCC hosted（実行）+ TinyCC freestanding（NO_STDLIB）
```

`make test-tcc` は `test-single-header-tcc` も含めて実行します。

### 5-1. NO_STDLIB構成の型定義（TinyCC対応）

`PYTHON_CODE_TO_C_NO_STDLIB` では、型（`size_t`/`int64_t` 等）を自前で定義して
いますが、TinyCCのヘッダは `stddef.h` でも `int64_t` を定義するため、そのまま
では typedef の再定義エラーになります。そこで:

- `__TINYC__` では **コンパイラ提供の型ヘッダ（stddef.h/stdint.h/stdbool.h）を使う**
  （`PYTHON_CODE_TO_C_USE_COMPILER_TYPES` を自動定義）
- 明示的に切り替えたい場合は `PYTHON_CODE_TO_C_USE_COMPILER_TYPES`（使う）/
  `PYTHON_CODE_TO_C_NO_COMPILER_HEADERS`（使わない）を定義
- `-nostdinc` 等でヘッダが無いカーネルは従来どおり自前定義

これにより、TinyCCのfreestanding（`-DPYTHON_CODE_TO_C_NO_STDLIB`）構成でも
単一ヘッダーがビルドでき、`nm -u` で hosted libc への参照が無いことも確認できます。

## 6. 品質ゲートとの関係

- 既定ビルド（C11＋厳格警告、`make full-build` / `make test`）の基準は不変です。
- `make test` には `test-hobbyos-libc`、`test-c99`、`test-single-header-c99` を追加しました。
- `make test-tcc` は `test-single-header-tcc` と差分コーパス全体をTinyCCで実行します
  （`TCC=/path/to/tcc` で場所を指定できます）。
- 検証環境（Ubuntu 24.04 / GCC 14）にtccが無い場合は、`apt-get download tcc` で
  パッケージを取得し、`dpkg-deb -x` で展開すればroot権限なしで使えます:

  ```sh
  cd /tmp && apt-get download tcc && dpkg-deb -x tcc_*.deb /tmp/tccroot
  make tcc TCC="/tmp/tccroot/usr/bin/tcc -B /tmp/tccroot/usr/lib/x86_64-linux-gnu/tcc"
  make test-tcc TCC="/tmp/tccroot/usr/bin/tcc -B /tmp/tccroot/usr/lib/x86_64-linux-gnu/tcc"
  ```

- TinyCCは `-MMD`/`-MP`（依存関係生成）を持たないため、tcc構成では `DEPFLAGS=` を
  空にしてビルドします（Makefile側で処理済み）。また `-D_GNU_SOURCE` 等を
  コマンドラインから渡しても再定義エラーにならないよう、ソース側の機能マクロ
  定義は `#ifndef` で保護しています。

