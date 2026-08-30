# 未使用ストレージバックエンド（退避）

このディレクトリのファイルは `Makefile` のビルド対象から**一切参照されていない**
ことを確認した上で `src/drivers/disk/` から退避したものです。

- `storage_image.c` / `storage_core.c` / `storage_meta.c` / `storage_meta.h` /
  `storage_core.h` / `disk_compat.c` / `disk_compat.h` / `storage_rom.c` /
  `storage_rom.h`

## 確認方法
`grep -rn "<filename>" Makefile` がすべて空振りであること、および他の `.c/.h`
からもこれらのヘッダがincludeされていないこと（`storage_meta.h`→`storage_core.h`
の内部依存を除く）をgrepで確認済みです。

## 経緯（推測）
「イメージファイルベースの新永続化層」への移行を試みた跡と思われますが、
実装が `storage.c`（ATA/IDE直叩き＋セクタ管理VFS）から切り替わることなく
放棄されています。

## 今後の扱い
- 復活させる予定がなければ、このディレクトリごと削除して問題ありません。
- 再利用する場合は、`storage.c` との役割分担（どちらが正なのか）を先に決めてから
  Makefileに組み込んでください。二重管理のまま両方生かすと再び同じ混乱が起きます。
