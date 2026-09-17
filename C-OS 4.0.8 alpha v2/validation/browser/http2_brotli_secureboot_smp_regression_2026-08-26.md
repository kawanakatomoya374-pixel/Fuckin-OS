# C-OS 4.0.8 alpha — HTTP/2・Brotli・Secure Boot SMP 回帰報告

## 実施範囲

本回帰では、既存の BearSSL/TCP/HTTP/1.1 経路を維持しながら、ALPN で明示的にネゴシエートされた場合だけ有効になる **nghttp2 ベースの単一 GET ストリーム HTTP/2 adapter** を統合した。adapter は TLS 接続後の ALPN 選択値が `h2` であることを確認してから client preface、SETTINGS、HPACK HEADERS、DATA、SETTINGS ACK、GOAWAY を処理する。ALPN が `http/1.1` または未選択の場合、既存の HTTP/1.1 経路へ留まる。

Brotli は既存の bounded response buffer と request-local decode workspace を使用する。HTTP/2 応答も、受信した `:status` と通常ヘッダを bounded synthetic response として既存の cookie・gzip・Brotli デコーダへ渡すため、content decoding の実装を二重化していない。

## 実装結果

| 項目 | 実装状態 | 安全条件 |
|---|---|---|
| ALPN | `tls_connect_prefer_http2()` が `h2`, `http/1.1` の順で提示 | 既存の `tls_connect()` は `http/1.1` のみを提示し、従来経路を保持 |
| HTTP/2 client | `src/kernel/drivers/http2_client.c` | HTTPS の idempotent GET、ALPN `h2`、新規 TLS 接続に限定 |
| HTTP/1.1 fallback | h2 transaction failure 時に接続を破棄して新規 HTTP/1.1 GET を実施 | GET/HEAD 相当の idempotent 要求に限定。POST は h2 を試行しない |
| HPACK / frame 処理 | vendored nghttp2 1.64.0 を使用 | C-OS kmalloc/kfree/krealloc allocator を明示指定 |
| Brotli | `Content-Encoding: br` を既存 bounded decode pipeline へ統合 | 出力容量超過・decode failure は安全に要求失敗へ遷移 |
| Cookie | h2 request に既存 cookie jar を送出し、h2 response の `Set-Cookie` を同一 jar へ保存 | pseudo-header と通常 header の順序を分離 |
| HTTP/2 reuse/multiplexing | **未実装** | 成功した単一 stream 後に GOAWAY を送信して transport を閉じる |
| 複数同時 TLS fetch | **未有効化** | `g_http_transport_busy` は現時点で保持。network dispatcher の再入性監査後にのみ解除可能 |

> **重要な境界:** 今回「HTTP/2 実装済み」と呼べる範囲は、ALPN `h2` 接続における実際の単一 GET stream である。複数 stream、shared h2 connection、HTTP/2 priority、同時 subresource 取得、並列 TLS worker はまだ有効化していない。

## ランタイム検証

制御された in-guest 診断ビルドを作成し、厳格 Secure Boot UEFI/QEMU で `https://nghttp2.org/httpbin/brotli` を通常の `http_get()` で取得した。診断 worker は production artifact に含まれず、最終 `build/kernel.elf` について `[HTTP-TEST]` string が存在しないことを確認済みである。

| 検証 | シリアル証跡 | 判定 |
|---|---|---|
| TLS ALPN | `[TLS] Connected for nghttp2.org ALPN=h2` | Pass |
| HTTP/2 response | `[HTTP/2] Single-stream response received, status: 200, length: 635` | Pass |
| Brotli runtime body | `Content-Encoding: br` を確認し、decoded body 198 bytes | Pass |
| End-to-end | `[HTTP-TEST] PASS: HTTP/2+Brotli status=200 bytes=198` | Pass |
| Secure Boot image signing | `sbverify` が C-OS Development Secure Boot signature を確認 | Pass |

検証中、`:status` pseudo-header を synthetic status line へ保存せず、共通 HTTP decoder が次の header 名を status と解釈して `0` を上書きする問題を発見した。`:status` を `HTTP/2 200\r\n` として直列化する修正後、上記の ALPN h2 + Brotli 実機試験は成功した。さらに diagnostics は `used_http2` フラグを要求するようにし、HTTP/1.1 fallback を HTTP/2 成功と誤認しない。

## 最終通常Secure Boot artifact

通常 artifact は `HTTP_RUNTIME_SMOKE=0` で再ビルド・再署名済みである。診断専用 ISO は別名で残しているが、最終配布対象は下表の通常 artifact である。

| Artifact | SHA-256 | 備考 |
|---|---|---|
| `C-OS_4.0.8_alpha.iso` | `3543b5c6270439ac23dfef30a3584fe7f115fbcfba979aa62de53edd3e318594` | 通常 hybrid ISO |
| `build/secureboot/C-OS_4.0.8_alpha_secure.iso` | `b5a21dbecf9baab8c6b1147532125a1c4308083f0e3bcd37e7806e6a0a13a276` | 最終 UEFI-first Secure Boot ISO |
| `build/secureboot/BOOTX64.EFI` | `df4f1d6675742f86b7e6f675a0de475b4903af831c9703ae4e75d5a5f46dd9f0` | 開発証明書で署名済み |

## 厳格 Secure Boot SMP 回帰

最終通常署名 ISO を `tools/run_qemu_c-os.sh --secure-boot --cpus N` で検証した。ランチャーは q35、TCG multi-thread、`-cpu max,enforce`、2 GiB、strict boot、OVMF Secure Boot、EHCI USB keyboard/mouse、AC97、E1000、QMP、guest error trace を含む。

| CPU 構成 | Online CPU 証跡 | GUI / scheduler | AC97 | EHCI TinyUSB | E1000 | 重大例外 |
|---|---|---|---|---|---|---|
| SMP2 | `Online CPUs=2` | GUI desktop 到達、preemption enabled | initialized | service loop active | RX/TX ring + runtime IRQ active | PANIC / #PF / #GP / triple fault / OOM なし |
| SMP4 | `Online CPUs=4` | GUI desktop 到達、preemption enabled | initialized | service loop active | RX/TX ring + runtime IRQ active | PANIC / #PF / #GP / triple fault / OOM なし |
| SMP8 | `Online CPUs=8` | GUI desktop 到達、preemption enabled | initialized | service loop active | RX/TX ring + runtime IRQ active | PANIC / #PF / #GP / triple fault / OOM なし |

QEMU debug log には `0xFED40000` 近傍の既知 MMIO unmapped read diagnostic が残るが、guest exception や triple fault ではない。上記全構成で QMP `query-status` は `running` を返した。

## 主要証跡

| ファイル | 内容 |
|---|---|
| `validation/browser/http2_brotli_secureboot_smp2_runtime_2026-08-26.md` | in-guest h2/Brotli 詳細 |
| `validation/http2_brotli_smp2_serial.log` | 成功した h2/Brotli 診断ログ |
| `validation/regression_smp2_final_serial.log` | 最終通常 ISO の SMP2 回帰 |
| `validation/regression_smp4_serial.log` | 最終通常 ISO の SMP4 回帰 |
| `validation/regression_smp8_serial.log` | 最終通常 ISO の SMP8 回帰 |
| `validation/runtime_smp2_gui_observations.md` | Secure Boot GUI 視覚確認証跡 |
| `build/secureboot/manifest.txt` | 署名・El Torito・hash manifest |

## 次の安全な作業

並列 TLS を有効化する前に、`net_poll()`、E1000 RX/TX、DHCP/DNS、ARP、TCP timer などの global state を single network-dispatch ownership へ整理する必要がある。その後、bounded fetch worker pool を導入し、h2 session の stream table・per-stream flow control・RST_STREAM を段階的に追加できる。現在は誤って `g_http_transport_busy` を外すより、安全な単一 h2 stream と HTTP/1.1 fallback を維持する方針が正しい。
