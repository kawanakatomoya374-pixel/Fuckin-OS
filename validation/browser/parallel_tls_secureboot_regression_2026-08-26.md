# C-OS 4.0.8 alpha — 限定並列TLS・Secure Boot回帰レポート

**対象日:** 2026-08-26  
**対象成果物:** `C-OS_4.0.8_alpha.iso` および `build/secureboot/C-OS_4.0.8_alpha_secure.iso`  
**通常成果物の診断設定:** `HTTP_RUNTIME_SMOKE=0`

## 結論

C-OSのHTTP transportを、従来の**全リクエスト直列化**から、最大二本の独立したTCP/TLS/HTTP transportを同時に許可する**bounded parallel transport**へ移行した。二本の512 KiB-stack NetSurf HTTP workerを追加し、名前解決は単一のpending hostnameではなく、DNS transaction IDで照合される最大八件の要求表へ変更した。

診断専用のSecure Boot SMP2 QEMU実行では、Cloudflareの `https://www.cloudflare.com/cdn-cgi/trace` に対する二本のHTTPS要求が、いずれも ALPN `h2`、nghttp2 single-stream response、Brotli復号、HTTP 200で完了した。HTTP semaphoreの実測 peak は **2** であり、二つの要求が単に連続成功しただけではなく、同時active transportとして動作したことを確認した。

> 通常成果物では診断workerをコンパイルから除外している。最終の署名済みSecure Boot ISOは、同一の厳格QEMU構成で SMP2・SMP4・SMP8 を回帰し、全構成でGUI、AC97、EHCI/TinyUSB、E1000、MP3初期化、preemption有効化まで到達した。PANIC、#PF、#GP、triple fault、OOMは検出されなかった。

## 実装内容

| 項目 | 実装 | 安全上の要点 |
|---|---|---|
| Network dispatcher | `net_poll()` を単一所有・一回bounded passへ変更 | 同時のE1000 RX、DHCP、TCP ingress解析を禁止。所有権を取れないworkerはyield経路へ戻る。|
| E1000 | TX/RX descriptor ringに短いspinlockを追加 | 複数transportの送信要求がtail更新を競合しない。|
| ARP | table lookup/insertをlockで保護 | transport workerのlookupとRX ingressによる学習を分離。|
| DNS | 最大8件のrequest table、ID照合、request-local hostname/result/state | 並列resolverが応答を取り違えない。terminal resultは待機callerが消費するまで再利用しない。|
| HTTP transport | 全体mutexを最大2件のCAS semaphoreへ置換 | clientごとのsocket/TLS/receive/decode workspaceは独立。cookie jarとkeep-alive metadataは専用lockを維持。|
| NetSurf fetch | 二本の`netsurf_http*` worker、各512 KiB stack | NetSurf callback、DOM/CSS mutation、描画は従来どおりGUI owner threadのみ。|
| 検証計測 | active/peak transport統計を追加 | 診断workerが二本の同時active transportを実測可能。通常起動ではネットワーク要求を実行しない。|

## 実機QEMU検証

### 並列TLS・HTTP/2・Brotli

診断ISOは `HTTP_RUNTIME_SMOKE=1` でのみ作成した。起動後、二つの独立した512 KiB workerが同時にHTTPS GETを開始した。検証先はホスト側でもHTTP/2 200および `Content-Encoding: br` を確認したCloudflare endpointである。

| 成功条件 | 結果 | 証跡 |
|---|---:|---|
| Secure Boot UEFI起動 | PASS | 署名ISOでのQEMU起動 |
| SMP2 Online CPU | PASS | `Online CPUs=2` |
| DNS resolver | PASS | DHCP DNS更新後の `www.cloudflare.com` 解決 |
| TLS ALPN | PASS | 両workerで `ALPN=h2` |
| HTTP/2 | PASS | 両workerで `Single-stream response received, status: 200` |
| Brotli | PASS | 両workerは `Content-Encoding: br` を必須条件として成功 |
| 二本の同時active transport | PASS | `[HTTP-TEST] ... peak=2` |
| 例外 | PASS | PANIC、#PF、#GP、triple fault、OOMなし |

診断ログの最終成功行は次のとおりである。

```text
[HTTP-TEST] parallel worker=0 PASS
[HTTP-TEST] parallel worker=1 PASS
[HTTP-TEST] PASS: two concurrent HTTP/2+Brotli requests; peak=2
```

### 最終通常署名ISOのSMP回帰

| 構成 | Online CPU | GUI / preemption | AC97 | EHCI/TinyUSB | E1000 | 異常 |
|---|---:|---|---|---|---|---|
| Secure Boot SMP2 | 2 | PASS | PASS | PASS | PASS | PANIC/#PF/#GP/triple/OOMなし |
| Secure Boot SMP4 | 4 | PASS | PASS | PASS | PASS | PANIC/#PF/#GP/triple/OOMなし |
| Secure Boot SMP8 | 8 | PASS | PASS | PASS | PASS | PANIC/#PF/#GP/triple/OOMなし |

回帰はq35、TCG multi-thread、2 GiB、AC97、EHCI USB HID、E1000、QMP/debug、`-strict-boot` を含むプロジェクト標準の厳格ランチャーで実行した。

## 主な変更ファイル

| ファイル | 変更内容 |
|---|---|
| `src/kernel/drivers/net.c`, `net.h` | dispatcher単一所有化、dispatcher統計 |
| `src/kernel/drivers/e1000.c` | TX/RX ring同期 |
| `src/kernel/drivers/arp.c` | ARP table同期 |
| `src/kernel/drivers/dns.c` | bounded request-correlated DNS resolver |
| `src/kernel/drivers/http.c`, `http.h` | 最大二本transport semaphore、peak統計 |
| `src/kernel/drivers/tcp.c` | 診断時のみのRXトレース（通常ビルドでは無効） |
| `src/netsurf/cos_fetch_http.c` | 512 KiB stackの二本worker pool |
| `src/kernel/kernel.c` | 診断専用の二本並列TLS/HTTP2/Brotli smoke worker |

## 残存制約と次段階

| 項目 | 現状 | 次段階 |
|---|---|---|
| HTTP/2 multiplexing | **未実装**。各h2接続は一streamのみ。二本の独立接続を並列にした。 | request-local nghttp2 sessionへbounded stream table、flow control、cancel/GOAWAY処理を追加する。|
| 同時transport数 | **最大2本**。意図的な安全上限。 | memory・descriptor・TCP再送の負荷測定後、最大4へ段階的に検討する。|
| NetSurf GUI実操作 | worker poolはビルド統合済みだが、GUIから複数subresourceを読む自動操作は未実施。 | USB HID/QMP入力またはブラウザ内self-testでHTML+CSS+画像の二本並列fetchを可視化する。|
| 完全streaming | response全体をclient bufferへ受信してから、owner threadへ12 KiB slice配信する方式。 | HTTP bodyのtrue streamingとprogressive decoderを設計する。|
| scheduler | APは依然として従属workerモデル。 | per-CPU scheduler化は別の大規模・高リスク課題として分離する。|

## 添付証跡

- `validation/parallel_tls_peak_smp2_serial.log`: 実測 `peak=2` の並列TLS/HTTP2/Brotli成功ログ。
- `validation/final_parallel_tls_smp2_serial.log`: 最終通常署名ISOのSMP2回帰。
- `validation/final_parallel_tls_smp4_serial.log`: 最終通常署名ISOのSMP4回帰。
- `validation/final_parallel_tls_smp8_serial.log`: 最終通常署名ISOのSMP8回帰。
- `build/secureboot/C-OS_4.0.8_alpha_secure.iso`: 診断workerなしの最終署名ISO。

## 参照

[1] [Cloudflare Trace endpoint](https://www.cloudflare.com/cdn-cgi/trace) — 実機HTTP/2+Brotli診断先。

[2] [RFC 9113: HTTP/2](https://www.rfc-editor.org/rfc/rfc9113) — HTTP/2 connection/stream model。

[3] [RFC 7932: Brotli Compressed Data Format](https://www.rfc-editor.org/rfc/rfc7932) — Brotli content coding。
