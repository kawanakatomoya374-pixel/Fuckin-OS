# HTTP/2・Brotli・並列TLS導入調査 — 2026-08-26

## 公式資料からの重要事項

| 項目 | 確認事項 | C-OSへの影響 |
|---|---|---|
| HTTP/2 | HTTP/2は単一接続上で複数の独立streamをインターリーブし、HEADERS/DATA、SETTINGS、WINDOW_UPDATE等のframeで通信する。 | HTTP/1.1クライアントへ単に`Upgrade`ヘッダを足すだけでは不十分。接続単位のframe parser、stream table、flow-control、HPACK状態が必要。 |
| TLS ALPN | HTTPS上のHTTP/2はALPNの`h2`で協調する。`h2c`はTLS ALPNで送信してはならない。 | BearSSLラッパーにALPN提案・選択結果取得APIを追加し、選択された場合のみHTTP/2 client prefaceへ遷移する必要がある。 |
| HTTP/2 frame size | 全実装は少なくとも16,384 byteまでのframe payloadを処理でき、9-byte frame headerを使用する。 | 既存の16 KiB HTTP stagingと整合するが、frame header／partial payloadを跨ぐstate machineが必要。 |
| HPACK | field compressionは接続単位でstatefulで、dynamic tableの初期上限は4,096 byte。HEADERS/PUSH_PROMISE/CONTINUATIONを完全に復元できない場合は接続errorとなる。 | 独自の部分実装は危険。nghttp2のlib-onlyを移植するか、HPACK decoder/encoderを含む十分に検証された実装を採用する。 |
| Brotli | Brotli decoder APIはone-shotとstreaming decodeを提供し、入力を過剰消費しない。streaming APIは`NEEDS_MORE_INPUT`／`NEEDS_MORE_OUTPUT`を返す。 | HTTP Content-Encoding `br`はrequest-local decoder instanceとbounded output bufferで、既存の段階的NetSurf配信と自然に接続できる。 |
| nghttp2 | Cの再利用可能なHTTP/2 framing libraryで、HPACK encoder/decoderを公開APIとして提供する。lib-onlyビルドが可能。 | freestanding allocator、socket/TLS send/recv callback、BearSSL ALPNのアダプタを作成して組み込む候補として最有力。通常のhostアプリ依存は移植しない。 |

## ローカル実装の監査

| 現状 | 発見 | 結論 |
|---|---|---|
| HTTP | `g_http_transport_busy`、静的16 KiB受信バッファ、共有Keep-Alive pool、静的chunk/gzip workspaceを使用。 | 現在は安全な直列化を優先しており、複数workerを有効化してはならない。 |
| TCP | ソケットリストと動的ポートカウンタはglobalで、同期保護なし。 | 並列TLS／HTTPにはsocket listとport割当のロック／IRQ-safe化が必要。 |
| TLS | `tls_session`自体はsession-local socket・BearSSL client/x509/iobufを持つが、`tls_drive_network()`のpoll/yieldカウンタがstatic global。公開ALPN APIはない。 | ALPNとTLS sessionを安全に並列化するには、poll/yield状態をsession-localへ移し、TCP安全化後にworkerを増やす必要がある。 |
| Brotli/HTTP2資産 | C-OSツリーにBrotli decoder、nghttp2、HPACK実装は存在しない。OpenSSLソースにはALPN実装があるが、稼働TLSはBearSSLラッパー。 | OpenSSLを新規に動的導入せず、既存BearSSLへALPNを追加し、Brotli／nghttp2をvendorしてfreestandingに適合させる。 |

## 段階導入順序

1. TCP socket list、local port allocator、TLS drive state、HTTP compression workspaceをrequest-localまたはlock保護へ移す。
2. Google Brotli decoderをvendorし、カスタム`kmalloc/kfree` allocatorと上限付きstream decoderラッパーを実装する。
3. `Accept-Encoding: br, gzip`と`Content-Encoding: br`をHTTP/1.1経路へ追加し、Brotliの回帰fixtureで検証する。
4. BearSSLへALPN (`h2`,`http/1.1`)を追加し、選択結果をHTTP層へ露出する。HTTP/1.1 fallbackを必ず維持する。
5. nghttp2 lib-onlyをvendorし、single connection / single streamのHTTP/2 framingから開始する。
6. connection-local HPACK／stream table／flow-controlを追加して、同一originの複数subresourceを限定並列streamへ移す。
7. 上位のNetSurf fetch/XHRとGUI owner-thread callbackを維持したまま、SMP2/4/8 Secure Boot UEFI回帰を実施する。

## 参照

1. RFC 9113, HTTP/2: https://www.rfc-editor.org/rfc/rfc9113.html
2. RFC 7932, Brotli Compressed Data Format: https://www.rfc-editor.org/rfc/rfc7932
3. nghttp2 documentation: https://nghttp2.org/documentation/package_README.html
4. Brotli decoder API: https://brotli.org/decode.html
