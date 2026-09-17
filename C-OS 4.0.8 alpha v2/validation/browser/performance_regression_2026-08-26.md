# C-OS Browser Performance Regression — 2026-08-26

## 実装した性能改善

| 層 | 実装 | 期待する効果 |
|---|---|---|
| HTTP owner-thread delivery | HTTP転送ワーカーが完了した応答を、NetSurfへ1 GUI pass当たり最大12 KiBの`FETCH_DATA`として段階配信するよう変更した | 巨大HTML、CSS、JavaScript本文を一括コールバックで処理してGUI入力・描画を停止させる経路を除去する |
| 応答寿命管理 | HTTP応答、CP932→UTF-8変換バッファ、リダイレクト`<base>`注入バッファを、最後の`FETCH_FINISHED`までfetch contextが所有するよう変更した | 段階配信中のuse-after-freeを防ぎ、abort時もcontext破棄で回収する |
| 表示・画像 | NetSurfの拡大縮小／binary透明bitmapパスを、クリップ済みバックバッファ直書きへ変更した | ピクセルごとの`vga_set_pixel()`呼出し、再クリップ、バッファ確認を除去する |
| GUI/SMP | 面積16,384 px以上の矩形を、全オンラインCPU数と同数の横帯へ分け、AP投入後にBSPも1帯を処理する方式へ変更した | SMP2/4/8でBSP待機をなくし、条件を満たす大規模fillを2/4/8本の同時タイルとして処理する |
| QuickJS/DOM/CSS | DocumentFragment、汎用Node tree mutation、位置・境界線・文字組・overflow・z-index等のCSSOM style反映を追加した | DOM構築とstyle更新を実libdom mutation／再box化へ接続し、UIライブラリの基本操作を増やす |

## 回帰資産

| 資産 | 内容 | 検証状態 |
|---|---|---|
| `validation/browser/quickjs_dom_css_performance_selftest.html` | 160ノードのDocumentFragment挿入、selector、classList、CSSOM、Event、Storage、fetch/XHR機能検査 | C-OS独自ストレージへ`/browser/quickjs_dom_css_performance_selftest.html`として投入済み |
| `validation/browser_perf_storage.img` | 上記HTMLを含む512 MiB C-OSストレージイメージ | primary/backup catalogとCRCを検証済み（2エントリ） |
| `validation/browser_perf_smp/` | Secure Boot UEFIのSMP2/4/8シリアル・debug・launcherログ | 下記の起動回帰を記録済み |

## Secure Boot UEFI SMP回帰

QEMUはq35、TCG multi-thread、CPU機能強制、2 GiB RAM、AC97、EHCI＋USBキーボード／マウス、E1000、strict boot、QMP、debug例外トレースを有効にして実行した。`-no-shutdown`のため、各起動は30秒で外部タイムアウト停止した。これはGUI稼働を維持する意図した終了であり、ブート失敗ではない。

| ケース | Online CPUs | GUI | AC97 | TinyUSB EHCI | E1000 | MP3 backend | 致命的例外 |
|---|---:|---|---|---|---|---|---|
| SMP2 | 2 | PASS | PASS | PASS | PASS | PASS | なし |
| SMP4 | 4 | PASS | PASS | PASS | PASS | PASS | なし |
| SMP8 | 8 | PASS | PASS | PASS | PASS | PASS | なし |

確認したログマーカーは、`[SMP] Online CPUs=N`、`[GUI] Boot complete - drawing desktop`、`[AC97] audio device initialized`、`[USB] TinyUSB non-blocking service loop active`、`[E1000] Device initialized successfully`、`[MP3] backend initialized`である。`PANIC`、Page Fault、General Protection、Triple Fault、OOMのマーカーは検出されなかった。

## 計測と既知の制約

NetSurfブラウザはすでに、`[NetSurf] 受信から完全表示まで ...ms`を、応答受信完了後の最初のpost-layout redrawで記録する。段階配信により、この計測は一括body callbackで歪められず、通信・DOM/CSS処理・最初の画面表示の時間をより分離しやすくなる。

> HTTP/2、Brotli、複数同時TLS転送は未実装である。下位HTTPスタックは共有TCP/TLS/gzipワークスペースとグローバル転送gateを使用しており、単にHTTPワーカー数だけ増やすとレスポンス・Cookie・接続状態を破壊する。そのため、今回の安全な高速化は、Keep-Alive再利用を保ったままGUI停止を除去する段階配信と、描画ホットパスの直接バックバッファ化に限定している。真の並列サブリソース取得は、request-local transport stateへの分解後に実装する必要がある。
