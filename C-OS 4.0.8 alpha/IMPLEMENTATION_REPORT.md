# C-OS 4.0.8 ブラウザ・OS安定化 実装／検証レポート

## 概要

添付ソースを基に、NetSurf／QuickJSブラウザのJavaScript・DOM・通信基盤、描画経路、GUI、音声、スタック、ACPICA、TinyUSB、HTTP Downloaderを更新した。最終成果物はクリーンビルド済みのBIOS起動ISOであり、SMP 8コア、AC’97、E1000、EHCIを持つQEMU構成で起動を確認している。

## 主な実装内容

| 分野 | 実装内容 | 状態 |
|---|---|---|
| QuickJS／DOM | `fetch`、`XMLHttpRequest`、origin分離した`localStorage`／`sessionStorage`、`style`、`classList`、DOM要素同一性キャッシュ、実DOM反映型`innerHTML`を統合した。ページ破棄時にはJS要求とDOMラッパーを確実に解放する。 | 実装・全体ビルド済み |
| NetSurf統合 | ページライフサイクルとQuickJSを接続し、HTTP要求完了をGUIスレッドへポンプする。PNG・JPEG・BMP MIMEを安全なコンテンツハンドラ経路へ登録し、SIMD画像転送を追加した。 | 実装・全体ビルド済み |
| HTTP | gzip展開出力が上限に達しても有効な先頭HTMLを保持する救済経路、keep-alive、受信から完全表示までの計測ログを追加した。 | 実装・全体ビルド済み |
| 描画／GUI | XRGB8888バックバッファ、dirty矩形BitBlt、AVX2/SSE2変換、PS/2入力ポーリング除去、ドラッグ時の再描画要求を追加した。スタートメニューの画面外はみ出しとヒットテスト崩れも修正した。 | 実装・全体ビルド済み |
| メモリ／スタック | サイズクラスの小規模割当キャッシュを強化し、C-OSスレッド既定スタックとGUIメインスタックを512KBへ統一した。QuickJS再帰上限は128KBのまま保持し、ネイティブ処理用ヘッドルームを確保する。 | 実装・SMP起動確認済み |
| ネットワーク | E1000のDMAリングをページバック化し、legacy PIC上のIRQ11アサートで起動停止する回帰を検出して、安定な協調受信経路へ戻した。 | SMP起動確認済み |
| AC’97 | DMAリセット待機、状態クリア、低遅延1024ワードPCMバッファ、DMA開始診断を追加した。起動自己診断でPCM DMAのBDL・LVI・CR・SRを確認した。 | QEMU DMA開始確認済み |
| ACPICA | 以前は永久に無効だったACPICA OSサービス層を有効化した。テーブルロード、イベントフック、EC、GPE／固定イベントフックを接続し、詳細AMLトレースを無効化して起動遅延を除去した。 | QEMU初期化確認済み |
| TinyUSB | EHCI HCDを明示的に有効化し、HIDに加えMSCホストをリンクした。MSCのマウント／アンマウント・ジオメトリ情報ブリッジとGUI状態表示を追加した。 | EHCI初期化・全体リンク確認済み |
| オフラインHTML | HTTP DownloaderがHTML MIMEを`offline.html`として保存し、保存直後に`file://`経由でNetSurfへ渡す事前読み込み・オフライン実行経路を追加した。 | 実装・全体ビルド済み |

## DOM実装と回帰検証

QuickJS-NGとNetSurf/libdomのDOMブリッジについて、Node、Element、CharacterData、MutationObserver、Range、Selection、Shadow DOM、DOMParser、XMLSerializer、DOMExceptionの主要経路を実装・拡張した。`dataset`はQuickJS Proxyによるlive反映、MutationObserverは複数observerの独立キューとmicrotask通知、CharacterDataは変更前のlibdom文字列をCバッファへ複製する旧値保持、Shadow DOMはslot割当・`assignedNodes()`・`assignedSlot`・slotchange通知を備える。

| QEMU自己テスト | 結果 |
|---|---|
| `quickjs_range_selftest.html` | `RANGE_PASS` |
| `quickjs_selection_selftest.html` | `SELECTION_PASS` |
| `quickjs_shadow_slot_selftest.html` | `SHADOW_SLOT_PASS` |
| `quickjs_mutation_characterdata_selftest.html` | `MUTATION_CHARACTERDATA_PASS` |
| dataset、複数Observer、DOMParser、DOMException、Comment | 既存の各PASSを確認 |

上記の回帰実行では、ページフォルト、未処理例外、QuickJS参照カウントassert、filesystem read failedを検出していない。なお、本統合は主要サブセットの実装であり、**完全なWHATWG DOM仕様準拠ではない**。default slotの厳密な再配布、composed event retargeting、Rangeの部分選択・live更新、独立XML Documentモデル、全APIのWeb IDL brand check等は残存課題として`dom_full_inventory.md`に記録した。

## 最終検証

| 検証 | 結果 |
|---|---|
| `make clean && make -j2` | 成功。ISOを再生成。 |
| SMP 8コア起動 | 成功。`[SMP] Online CPUs=8`を確認。 |
| AC’97 | PCI検出、VRA対応、PCM DMA開始を確認。開始時ログは`LVI=1`、`CR=0x1D`、`SR=0x0000`。 |
| EHCI／TinyUSB | QEMUのEHCIをPCI検出し、TinyUSBホストスタック初期化を確認。 |
| ACPICA | ACPICAイベントフック、ACPIテーブル、AML名前空間、スマートバッテリ経路を確認。 |
| 安定性 | 最終QEMU実行ログに`FATAL`、`PANIC`、ページフォールト、未処理例外は検出されなかった。 |

## 既知の運用上の注意

本ISOはBIOS起動用である。今回のビルドではISOにUEFI El Toritoエントリが含まれることも検査したが、主たるQEMU検証構成はBIOS起動である。USBについてはEHCI/HID/MSCホストクラスとマウント通知までを接続したが、外付けドライブをFatFsの追加ボリュームとしてファイルマネージャへ公開するためのマルチボリューム・ブロックデバイス層は、今後の独立した拡張対象である。

また、legacy PIC上でE1000 IRQ11を即時有効化するとSMP起動を停止する回帰をQEMUで検出した。このため、受信は確実性を優先して協調的なリング排出へフォールバックしている。MSI/APICルーティングを整備した後に、NIC割込みを再度有効化するのが安全である。

## 収録物

- `C-OS_4.0.8_alpha.iso`: クリーンビルド済みのBIOS起動ISO。
- `dom_full_inventory.md`: DOM実装範囲、QEMU自己テスト、残存WHATWG差分の台帳。
- `C-OS_4_0_8_alpha_browser_stable_source.zip`: 変更済みソース、レポート、起動ログを含むソースアーカイブ（旧成果物）。
- `qemu_final_serial.log`: SMP 8コア・AC’97・EHCI構成での最終起動ログ。

> 詳細な設計調査メモはソースアーカイブ内の `research_notes.md` に収録している。
