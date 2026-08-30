# C-OS Browser Benchmark Plan — 2026-08-27

## 目的

QuickJSとNetSurfの改善は、ページ固有の画面表示だけでなく、通信、JavaScript、DOM mutation、DOM-to-box、resource decode、first post-layout paintを分離して測定する。Secure Boot UEFI・strict QEMU・同一のSMP数・同一storage imageを固定し、改善前後のログを比較する。

## 現状確認済みの計測経路

| 層 | 現在の計測・実装 | 今回追加すべき指標 |
|---|---|---|
| HTTP/TLS | `HTTP/PERF`、`TLS/PERF`、NetSurf fetch body/owner delivery time | navigation開始からHTML最終byte、resource別cache hit/miss、keep-alive reuse |
| NetSurf owner delivery | 大応答は最大12KiB/GUI passの`FETCH_DATA`段階配信 | chunk数、owner delivery総時間、post-layout first paintまでの時間 |
| QuickJS | page script byte数/complete/exception、GUI ownerによるtimer/microtask pump | script evaluate時間、microtask数と時間、DOM mutation/rebox集約回数 |
| DOM/CSS | `html_rebuild_layout_after_dom_mutation`と初回DOM-to-box | rebox request/coalesce/execute時間、layout時間、style変更回数 |
| Decode/paint | NetSurf redraw統計、bitmap plot、dirty BitBlt | PNG/JPEG/SVG decode時間、bitmap plot時間、dirty矩形面積、BitBlt tile時間 |
| GUI/SMP | 最終32bpp BitBltはBSP+AP tile dispatch | online CPU数、tile grid、AP CPU assignment、present時間 |

## 標準シナリオ

| ID | ページ | 主な検証対象 | 成功条件 |
|---|---|---|---|
| `local-dom-160` | `quickjs_dom_css_performance_selftest.html` | DocumentFragment、selector、style、Event、Storage | 自己テスト画面PASS、例外なし、JS/DOM/layout時間を出力 |
| `local-external-js` | `local_quickjs_file_selftest.html`＋外部module | file fetch、relative external JS、Storage、Event | 画面PASS、外部script評価時間を出力 |
| `google-home` | `https://www.google.com/` | TLS/h2、CSS/JS、icon/logo、form初期表示 | first paint、script exception、resource cache統計 |
| `wikipedia-page` | 一つの固定Wikipedia本文URL | 大HTML/CSS/画像・段階fetch・初期表示 | navigation→first paint、complete paint、GUI停止なし |

## 比較ルール

各シナリオをcold cacheとwarm cacheで少なくとも各3回実行し、中央値・最小値・最大値を記録する。外部ネットワークのRTTは変動するため、総時間だけで因果を主張せず、HTTP/TLS、script、layout、decode、paintの内部時刻を併記する。`PANIC`、`#PF`、`#GP`、OOM、QuickJS unhandled exceptionはrun failureとして別列に記録する。

## 現在のボトルネック仮説

1. Googleログでは独立resourceのTLS `connect+send_ms`が数秒単位となることがあり、HTTP/2がsingle streamであることとconnection reuse/parallel schedulingがページ完了時間を支配し得る。
2. Wikipediaの遅延は、HTMLを最終byteまで受けてからのNetSurf conversionだけでなく、CSS/JS/image resourceが直列transportを占有する影響を切り分ける必要がある。
3. DOM mutationはすでにowner-threadへのreboxを持つが、個別mutationでreboxが重ならないことを時間と回数で計測する必要がある。
4. GUI final presentはSMP tile化済みだが、page rendering自体はBSP ownerであり、APはDOM/NetSurf callbacksに入らない。この不変条件を崩さず、decode/surface copyの独立部分だけを並列化する。

## 実装順

1. monotonic timestampに基づく上記各stageの一回限り/URL別telemetryを追加する。
2. local DOM/external JS benchmarkをSecure Bootで実行し、QuickJS/DOM baselineを保存する。
3. Google/Wikipediaを同一固定URLで実行し、cold/warm resource/cache対比を保存する。
4. 測定値から最遅段階を一つ選び、一般経路の改善を実装する。URL patternだけのblockやGoogle/Wikipedia専用分岐は導入しない。
5. 改善後に同一QEMU profileで再測定する。
