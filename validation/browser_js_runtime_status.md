# Browser / QuickJS runtime status

最終確認日: 2026-08-25

## 確認済み

- Secure Boot + UEFI + SMP8 でC-OSは起動し、`Online CPUs=8`を出力する。
- scheduler診断でring3終了後にGUI PIDがrun queueから選択されることを確認した。
- WIN_BROWSER生成時にvisible/focused/active_windowが設定される。
- WIN_BROWSER生成時に`browser_initial_load_pending=true`が設定され、即時GUI invalidateが発行される。
- `draw_browser_app()`の初回data:文書ロードを同期`browser_load_via_real_netsurf()`から`cos_netsurf_load_url_sync_nowait()`へ変更した。
- QuickJS DOM/Event/Storage自己検証用data:文書はkernel.elfへ組み込まれることを確認した。
- 通常の差分ビルドとUEFI ISO生成は成功した。

## 未確認・未達

Secure Boot QEMU実行で、GUI上のNetSurf自己検証文書の`QuickJS DOM + Event + Storage PASS`表示と、serial上の実行完了マーカーはまだ取得できていない。検証用自動起動フックはビルド成果物へ組み込まれることを確認したが、起動試験では安定して発火しなかったため、通常ソースからは除去済み。

## 次の切り分け

通常のマウスクリック経路を使ったブラウザ起動を優先し、QEMU入力ヘルパーの座標・TinyUSB HID入力到達・GUIのWIN_BROWSERクリック処理を個別に確認する。次に、ブラウザ生成後の`browser_initial_load_pending`消費、`cos_netsurf_browser_open()`呼び出し、NetSurf content callback、QuickJS script evaluation、plotter redrawを各段階で1回ずつ記録する。成功条件は、画面上のPASS表示、DOM mutation後の再描画、serialのNetSurf/QuickJS完了ログ、Secure Boot + UEFI + SMP8でfaultなし、の全てを満たすこととする。
