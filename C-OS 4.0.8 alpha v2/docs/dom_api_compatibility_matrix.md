# QuickJS / NetSurf DOM API compatibility matrix

| 領域 | 現在の実装 | 実GUI検証 | 残存作業 |
|---|---|---:|---|
| ノード同一性 | ページ文脈ごとのlibdomノード→JSラッパーキャッシュ | 済み | キャッシュ飽和時の退避方針 |
| 基本検索 | `getElementById`、タグ検索、`querySelector('#id')`、単一タグ`querySelector` | ID検索は済み | class、属性、結合子、疑似クラス |
| 属性 | `get/set/has/removeAttribute`、`id`、`className` | set/hasは済み | dataset、名前空間属性 |
| CSS/クラス | 代表的styleアクセサ、`classList.add/remove/contains/toggle` | addは済み | CSSStyleDeclaration全体、DOMTokenListの厳密な例外規則 |
| DOMツリー | createElement/Text、append/remove/insert/replace、clone、親子・兄弟参照 | appendとparent/firstChildは済み | DocumentFragment、NodeList/HTMLCollectionのlive化、normalize |
| HTML | 代表的な安全属性を持つbounded fragment挿入 | plain text/挿入は済み | HTML標準フラグメントパーサ、innerHTMLシリアライザ |
| イベント | Elementのadd/removeEventListener、NetSurf target配送、ページlifecycle配送 | 未実行 | capture/bubble、once/passive、preventDefault、click/change GUI回帰 |
| 非同期API | timers、fetch/XHR、Storageのbounded実装 | 一部済み | transportの完全非ブロッキング化、Cookie |
| 描画反映 | DOM mutation→次フレームbrowser reformat通知 | 未実行 | dirty範囲の最小化、構造変更の再レイアウト回帰 |

このマトリクスで「実装済み」はソース接続済み、「実GUI検証」は実QEMU上でページスクリプトと表示が確認済みを意味する。両方を満たして初めてユーザー向けに動作保証できるものとして扱う。

## 2026-05-26 継続実装記録

`Element`/Text wrapperに`dispatchEvent()`を追加し、QuickJS globalへ`Event`および`CustomEvent` constructorを登録した。dispatchは実libdom node wrapperのlistener tableとNetSurfのDOM event dispatch経路を利用する。今回のビルドではコンパイル・リンクを確認済みだが、SMP8実GUI上で`new Event('click')`→`element.dispatchEvent()`→callback→画面反映の連鎖は未検証であるため、イベント行の「実GUI検証」は未完了のままとする。

style/classList、ノード同一性cache、createElement/appendChild、非同期fetch/XHR、Storageについては既存実装を再監査し、代表的な実GUI検証済み範囲と仕様上の残存制限を維持した。完全DOM APIという名称であっても、DocumentFragment、live NodeList/HTMLCollection、CSSStyleDeclaration全体、属性selector/結合子、HTML serializer、厳密なEvent cancellation semanticsは未達成である。

USBについては、C-OS polling contextに合わせてEHCI root attachおよびqTD completionのevent通知を非ISR扱いに整理し、pending control transfer FIFOを導入した。単一VMでのクリーン検証ではroot attach・`enum_new_device()`・初回SETUP投入までのログを取得したが、HID mount callback到達はまだ再現・証明できていない。複数診断VMの残留がSMP8起動を干渉していたため、今後は必ず単一VM・SMP8で検証する。
