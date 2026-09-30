# C-OS Browser Performance Design — 2026-08-26

## 現状の測定可能な事実

| 層 | 現状 | 主要な制約 |
|---|---|---|
| NetSurf HTTP fetch | 専用カーネルワーカーへ非同期投入し、GUIスレッドでは完了後の `FETCH_*` コールバックのみを行う | ワーカーは1本で、下位HTTPクライアントにもグローバル転送ゲートがあるため、サブリソースは実質直列 |
| HTTP transport | 16 KiB受信ステージング、HTTP/1.1 Keep-Aliveプール4本、DNS/TCP/TLSを再利用可能 | TCP/TLS、gzipワークスペース、Keep-Aliveプールが共有され、現時点で安全な並列転送は不可 |
| ページ表示 | `browser_window_redraw()`を実行し、ロード中のreformatを約30Hzに抑制 | 応答本文はNetSurfへ一括`FETCH_DATA`で配信されるため、巨大HTMLの処理が1 GUI passへ集中し得る |
| QuickJS | DOM mutationはGUI owner thread上で遅延rebox、イベント・Storage・fetch/XHR・DocumentFragmentを実DOMへ接続 | スクリプト実行と大規模DOM変更は同一フレームに集中し得る |
| GUI/plotter | dirty redrawとBitBlt、SMP大規模fill分割、opaque等倍bitmapのSIMDパスあり | 拡大縮小／alpha bitmapと大量小プリミティブは高コスト |

## 実装順序

1. **測定と応答配信を改善する。** HTTP往復時間、GUI ownerへの配信時間、最初のpost-layout表示時間をURL単位で記録する。既存の「受信から完全表示まで」ログを残し、各段階の原因を分離する。
2. **GUIをブロックしない配信にする。** 大きなHTML/CSS/JSを固定上限の`FETCH_DATA`片へ分け、GUI passごとに限定量をNetSurfへ渡す。応答メモリの寿命、変換バッファ、aborted fetchの破棄を文書化してから導入する。
3. **真の並列HTTPは下位再入可能化の後に行う。** 共有TCP/TLS/gzipワークスペースとグローバルgateをrequest-local化するまで、ワーカー数だけ増やさない。壊れたCookie/レスポンス混線より直列Keep-Aliveを優先する。
4. **QuickJS workを予算化する。** microtask/timerとDOM reboxのフレーム予算を可視化し、ページロード中は再formatと再描画の重複を抑える。
5. **描画を高速化する。** viewport clip、dirty generation、等倍不透明bitmap fast pathを保ち、CPUコアへ安全に分離できる大きな画像・fill変換だけをSMPジョブ化する。

## 安定性の不変条件

- NetSurf `FETCH_*` callbacks、libdom mutation、QuickJS execution、GUI presentationは同一owner threadに限定する。
- AP workerは互いに重ならないバックバッファ行範囲または独立scratch bufferだけを処理する。
- Secure Boot UEFIを主経路とし、SMP2・4・8でGUI・USB・E1000・AC97・ブラウザ初期化の回帰を行う。
- 未検証のHTTP/2、Brotli、複数同時TLS転送を「実装済み」とは扱わない。

## 検証指標

1. `HTTP transport complete → GUI FETCH delivery` の時間。
2. `navigation start → first post-layout redraw` と、既存の `受信から完全表示まで`。
3. 1フレームで処理した応答データ量とNetSurf callback回数。
4. QuickJS自己テスト（DOM、Event、Storage、DocumentFragment、CSSOM、fetch/XHR）。
5. Secure Boot UEFI SMP2/4/8でのOnline CPU数、GUI、USB、E1000、AC97、PANIC／#PF／#GP／Triple Fault不在。

> 真の通信並列化は、現段階では安全な小変更ではありません。まずは大応答の段階的owner-thread配信と計測を導入し、表示停止を除去した後で、request-local transportへの分解を行います。
