# Py2C on C-OS

このフォルダ (`/apps/Py2C`) は **Python Code to C (Py2C) Beta0.1 のソース一式**です。ファイルマネージャーで見て、C-OS Studio / Py2C Studio で開いて、**この OS の上で自分自身をビルド**できます。

## 使う (Terminal)
```
cd /bin
./python-code-to-c hello.py -o hello.c      # Python -> C
./python-code-to-c run hello.py             # 変換 -> tcc でコンパイル -> 実行
./python-code-to-c --supported              # 対応する構文の一覧
./python-code-to-c --self-test              # 同梱の例を一括確認
./python-code-to-c help
```
(`py2c` という名前は付けません: FAT ではそれが Py2C Studio (`Py2C.c-os`) と同じ名前になります。)`run` は、ランタイムライブラリ `/system/sdk/lib/libp2crt.a` があればそれを使い、なければこのフォルダの `src` からランタイムを一緒にコンパイルします。

## ビルドする
- **C-OS Studio**: `File > Open Folder` で `/apps/Py2C` を開き (`cosproj.txt` があるのでプロジェクトとして開きます)、Build (F7)。成果物は `/C-OS Studio/deliverables/python-code-to-c.c-os`。
- **Studio のターミナル**: `cd /apps/Py2C` して
  - `make` : `build/python-code-to-c.c-os` を作る
  - `make runtime` : `build/libp2crt.a` (変換した C のプログラムがリンクするランタイム) を作る
  - `make install` : `/bin/python-code-to-c.c-os` と `/system/sdk/lib/libp2crt.a` を入れ替える
- **Py2C Studio**: F6 は変換した C をこのランタイムとリンクして実行します。

`Makefile.gnu` は元の (GNU make 用) Makefile です。テスト (`tests/`) とドキュメント (`docs/`) もそのまま入っています。C-OS 向けに変えた箇所は `docs/py2c_cos_port.md` (OS のリポジトリ側) にまとめてあります: `P2C_COS_PORT` を定義すると、TLS を使わず、ソースの置き場所とプリビルドのランタイムの場所が C-OS のものになります。
