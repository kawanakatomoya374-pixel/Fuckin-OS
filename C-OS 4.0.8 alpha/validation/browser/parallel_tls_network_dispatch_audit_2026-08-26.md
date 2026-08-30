# 並列 TLS 解禁前の network dispatcher 監査 — 2026-08-26

## 結論

現時点で **複数 HTTP/TLS worker を同時に有効化してはならない**。TCP socket registry と HTTP request-local buffer、cookie jar、keep-alive metadata には限定的な同期化が追加されているが、packet ingress と global protocol state を所有する network dispatcher は再入不可である。

`g_http_transport_busy` を維持するのは性能未達ではなく、E1000 RX ring、ARP/UDP/DHCP/DNS、そして `net_poll()` 内の処理を同時に走らせて ring cursor や global DNS request state を破壊しないための正しい安全ゲートである。

## コード監査結果

| 層 | 現在の共有状態 | 同期状態 | 並列 worker への問題 |
|---|---|---|---|
| `net_poll()` | `e1000_poll()` → `dhcp_poll()` → `tcp_poll()` の順で全 protocol dispatcher を実行 | **lock なし** | 複数 worker が同時に呼ぶと ingress parser を並行実行する |
| E1000 TX | `e1000_dev->tx_tail`、descriptor、TX statistics | **lock なし** | 2 worker が同じ tail/descriptor を選ぶ可能性 |
| E1000 RX | `e1000_dev->rx_tail`、RX descriptor ownership、`eth_recv()` | IRQ pending bit は atomic、ring drain は **single-consumer lock なし** | 2 worker が同じ RX descriptor を処理・返却する可能性 |
| E1000 IRQ | `e1000_rx_irq_pending` 等 | pending bit は atomic | atomic bit は「一人だけが drain する」保証ではない |
| DNS | `dns_next_id`、`dns_pending_hostname`、固定 size cache、`dns_server` | **lock なし** | 同時 resolve が pending hostname を上書きし、応答を誤った hostname に cache する |
| DHCP | global `dhcp_client`、DNS server update | **lock なし** | DHCP transition と resolve/update が競合し得る |
| TCP registry | `tcp_sockets`、port allocation、socket state | registry lock あり | packet handler が lock 中に ACK を送るため、E1000 TX 自体は保護されない |
| HTTP metadata | cookie jar と keep-alive pool | spinlock あり | lower network layerの再入性を補えない |

## 実装済み保護が十分でない理由

TCP registry lock は socket lookup、rx buffer append、短い state mutation を守る。しかし `tcp_handle_packet()` は lock を保持したまま `tcp_send_packet()` を呼ぶ。`tcp_send_packet()` は最終的に E1000 TX tail を更新するが、E1000 の TX tail/descriptor には lock がない。したがって二つの TCP flow が registry lock の外側から同時に送信すれば、NIC ring は破損し得る。

同様に `e1000_poll()` は atomic exchange で pending flag を消費するが、RX drain 全体の所有権を mutex/CAS state machine で守っていない。片方が pending flag を取り、もう片方が IRQ により再設定された flag を取るタイミングでは、両者が `rx_tail` と RX descriptor を同時に更新し得る。DNS はさらに明確で、global `dns_pending_hostname` が one-outstanding-request 設計である。

## 並列化の安全な次段階

| 順序 | 必要な変更 | 完了条件 |
|---|---|---|
| 1 | `net_poll()` を owner-only dispatcher へ変更 | single BSP/service thread または network lock だけが E1000 RX、DHCP、ARP、UDP、TCP poll を実行 |
| 2 | E1000 TX/RX ring に明示的な single-consumer / producer synchronization を導入 | 送信tail・受信tail・descriptor ownership に同時更新がない |
| 3 | DNS を request table（ID、hostname、waiter、deadline）へ移行 | 複数hostnameの同時 resolve と response matching をテスト |
| 4 | worker は poll せず、dispatcher event/condition を待つ | workerごとの socket/TLS request state は lock-freeに近い所有モデルを維持 |
| 5 | bounded worker pool を 2 本から開始 | 異なる host の TLS GET を反復し、race detector相当の invariant logs とSMP2/4/8回帰を実施 |
| 6 | h2 stream table / flow control を導入 | single h2 connectionの複数streamを明示的に有効化 |

この監査により、現在の単一 h2 stream と HTTP/1.1 fallback は維持し、parallel worker と HTTP/2 multiplexing を未実装として扱う。これは安定性要求に合致する段階導入である。
