# C-OS HTTP/2・Brotli・並列TLS 実装設計 — 2026-08-26

## 目的

BrotliをHTTP/1.1で先行有効化し、TLS/HTTP/2/複数転送を一度に無防備に有効化しない。NetSurfとQuickJSのfetch/XHRはこれまでどおりGUI owner threadでcallbackとDOM変更を実行し、下位転送の並列化がGUI・libdom・QuickJS競合へ波及しないようにする。

## request-local transport model

```text
NetSurf / QuickJS request
  -> fetch context (owner-thread callbacks only)
  -> transport job (request-local http_client / response / decoder)
  -> connection lease (host, port, scheme, TLS session or HTTP/2 session)
  -> TCP socket (locked global-list insertion/removal, lock-protected ephemeral port)
```

各transport jobは次の状態を自身で保持する。

| 状態 | 現状 | 目標 |
|---|---|---|
| HTTP受信 staging | 静的16 KiB | `http_client_t`またはtransport jobごとの16 KiB。HTTP/2 frame parserも接続ごとの受信ringを持つ。 |
| response body | `http_client_t`ごと | 維持。NetSurfへの12 KiB owner-thread配信も維持。 |
| content decoder | gzipは静的workspace、Brotliはdecoder stateをrequest-local化可能 | Brotliはrequest-local state。gzipも段階的にrequest-local化し、並列化前に静的workspaceを廃止する。 |
| TLS session | BearSSL構造体はsession-local、network poll cadenceはglobal static | `poll_since_yield`などをsession-localに移す。ALPN proposal/selectionもsessionに保持。 |
| TCP global state | socket linked listとnext_portが無保護 | spinlock/IRQ-safe lockでcreate/destroy/list traversal/port assignmentを保護する。 |
| Keep-Alive | global pool、直列使用前提 | pool lockとlease state（idle/in-use）を追加。同一HTTP/1.1接続に同時requestを送らない。 |

## 段階的有効化

| 段階 | 機能 | 安全条件 | フォールバック |
|---|---|---|---|
| A | HTTP/1.1 `Content-Encoding: br` | Brotli decoderはrequest-local allocator、出力cap、chunked後の正しいdecode順序 | `gzip`またはidentity。br decode失敗は安全なエラー文書。 |
| B | TCP/TLS request-local化 | socket list・port・network cadence・gzip workspaceが共有競合を持たない | 直列workerへ戻す。 |
| C | HTTP/1.1並列接続 | hostあたり最大2、全体最大4。poolのlease取得のみ並列。 | wait queue。POST replayなし。 |
| D | TLS ALPN | `h2`と`http/1.1`を提案し、selectionを検証する。 | ALPN未選択/`http/1.1`なら既存HTTP/1.1。 |
| E | HTTP/2 single stream | client preface、SETTINGS ACK、HPACK、HEADERS、DATA、RST_STREAM、GOAWAYをnghttp2で処理。 | protocol/HPACK errorでは接続廃棄しHTTP/1.1へ再接続。 |
| F | HTTP/2 multi stream | connection-local stream table、flow control、per-origin上限、owner-thread delivery queue。 | stream errorはそのstreamのみ中止、connection errorはHTTP/1.1 fallback。 |

## HTTP/2採用方針

HTTP/2はHPACK dynamic tableとflow controlを接続単位で保持するため、独自の部分実装を避ける。公式nghttp2の**lib-only**をvendorし、freestanding adapterで以下を提供する。

1. `send_callback`: BearSSLのsession-local送信へ接続する。
2. `on_header`: response headerをstream-local metadataへ格納する。
3. `on_data_chunk_recv`: stream-local body bufferへ追記し、上位では既存12 KiB callback配信を使用する。
4. `on_stream_close`: NetSurf/QuickJS完了キューへメッセージを積む。
5. allocator hooks: `kmalloc/kfree`と上限付きstream bufferを使用する。

## 不変条件

- HTTP/2 frame/HPACK/TLS/TCPの処理はtransport workerで行い、NetSurfのFETCH callbackとQuickJS promise解決はGUI owner threadへ戻す。
- HTTP/1.1、TLSなしHTTP、gzip、identityの既存互換性を維持する。
- 失敗したALPN/HPACK/frameは接続を破棄し、破損状態をpoolへ戻さない。
- GET/HEADのみ自動fallback/retry対象とし、POST/フォーム送信は暗黙再送しない。
- Secure Boot UEFI、SMP2/4/8、USB、E1000、AC97、GUI起動回帰を各段階で実施する。
