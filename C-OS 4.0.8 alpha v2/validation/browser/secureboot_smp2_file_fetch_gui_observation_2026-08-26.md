# Secure Boot SMP2 file:// GUI observation — 2026-08-26

検証ISOは `COS_BROWSER_FILE_SMOKE=1` でビルドし、`validation/local_browser_dual_storage.img` を明示PCI IDE controller経由で接続した。C-OSは実機ログ上で ATA IDENTIFY (1024 MiB) と、sector 1048576 からの 512 MiB FatFs領域のmountを成功させた。host側でもそのFatFs領域の `/browser` に local QuickJS self-test と添付HTML三件が存在することを確認済みである。

QMP screendumpで得た `dual_storage_desktop.png` はSecure Boot SMP2のC-OS desktopを示す。画面上にはNetSurfアイコンが存在する。QMP `input-send-event` で同アイコン位置へのabsolute pointer clickを試行したが、8秒後の `dual_storage_browser_selftest.png` はdesktopから変化せず、カーソルも動かなかった。これは file fetch の失敗証拠ではなく、現時点ではこのQMP absolute pointer eventがC-OSの入力経路へ届かなかったことを示す。Browser windowおよびlocal self-test PASSはまだ未確認である。

次回はUSB HID/QMPの入力形式を切り分けるか、検証ビルド限定でdesktop到達後にBrowser windowを明示起動して、file:// fetch・外部script・QuickJSの実行証跡を取得する。


## 自動Browser起動後の再検証

`COS_BROWSER_FILE_SMOKE=1` 限定でdesktop最初の描画時に通常の `WIN_BROWSER` 作成経路を一回だけ呼ぶ差分を加えた。Secure Boot SMP2の実画面 `secureboot_smp2_file_quickjs_selftest.png` では、Browser windowのタイトル `NetSurf file:// self-test` とアドレスバー `file:///browser/local_quickjs_file_selftest.html` が明確に表示された。シリアルログにも `GUI triggered pipeline`、NetSurf初期化、QuickJS context作成、`1 + 2` の `QJS Result: 3`、`browser_window_navigate` による当該file URLロード開始が記録された。

ただし本文レンダリング領域は白紙であり、file fetchの完了・HTMLコンテンツ変換・相対external scriptの取得・self-test PASSは未確認である。従って現時点の成果は、**C-OS FatFs上のfile URLを実NetSurf browser_windowまで渡した実機証跡**までであり、ローカルHTML実行成功の証明ではない。次の作業は `cos_fetch_file.c` のfetch開始/headers/data/finishedの進行、およびNetSurf schedule pumpが開始後も回るかをログ計測で追跡することである。


## scheduler／USB停滞の切り分けとfile://実行到達

GUI owner loopの最初の `usb_poll()` は、EHCIのasync／periodic link traversalが不正な循環へ入り得るため、無限に戻らないことをシリアルのenter markerで確認した。TinyUSB EHCIの両traversalに256 nodeの上限を加え、警告は各種類一度だけ出すようにした。これは列挙の正しさを完成扱いにするものではないが、USB不調がGUI／NetSurfを永久停止させる致命的な経路を除去する安全策である。上限後の実機ログでは `[USB] Keyboard connected.`、heartbeat、GUI owner loopの継続が確認できた。

その結果、Secure Boot SMP2で `file:///browser/local_quickjs_file_selftest.html` はstorage-backed file fetcherにより1895 bytes読出し・`FETCH_FINISHED`へ到達し、相対外部script `local_quickjs_file_module.js` は94 bytesで別file fetchとして完了した。続いてQuickJSが外部scriptとinline script（1355 bytes）を実行し、NetSurfの `DOM to box conversion complete` とcontent openを記録した。従って、**C-OS FatFs → file: fetcher → NetSurf content/DOM → relative external JS fetch → QuickJS execute** の実行連鎖は実機ログで確認済みである。

同時に、`file://` GUI分岐が `fs_list_dir()` の静的バッファを常に成功扱いして実行済みHTMLをDirectory listingで上書きするバグを修正した。修正後の実画面ではDirectory listingは消え、NetSurf 3.11 statusとfile URLが表示される。しかし本文は依然白紙である。シリアルはDOM conversion後のNetSurf redrawが `plots rect/text/bitmap=1/5/0` まで到達したことを示すため、次の未解決点はfile fetch／QuickJSではなく、**NetSurf text plotterの座標・clip・backbuffer反映**である。画面上でPASS文を表示する証跡はまだ未取得である。


## document.title bridge の実機結果

QuickJS bridgeに実libdom-backed `document.title` getter/setterを追加し、setterはBrowser bridgeへタイトル変更を通知するようにした。Secure Boot SMP2の最新実機画面では、Browser title bar が `C-OS local HTML QuickJS ERROR` へ変化した。従って、**inline QuickJSが実際にcatch節まで実行され、script-driven `document.title` mutationがlibdom／GUIへ反映された**ことは画面で確認できた。

同時に、既存のlocal self-testはtry節内で例外を捕捉していることが判明した。本文text plotが白紙のままなので画面から例外内容を読めず、現時点ではこのself-testをPASSと主張できない。次は検証fixtureのcatchで`console.error`へ実エラー文字列を出し、外部script／DocumentFragment／Storage／Eventのどのgeneric DOM APIが失敗しているかを特定して修正する。


## Storage origin と layout の実機切り分け

`js_newthread()` が受け取る `doc_priv` はlibdom documentではなくNetSurf content objectであることを確認した。QuickJS backendはこのcontent objectから取得したページ本体URLをscript実行直前にWeb APIへ渡す方式へ変更し、C-OS storage-backed `file:` URLには `file://c-os-storage` という明示的なbounded local originを割り当てた。Secure Boot SMP2の実機ログで、`file:///browser/local_quickjs_file_selftest.html` に対してこのorigin keyが設定されたこと、相対外部script（94 bytes）とinline script（1416 bytes）が完走し、Storage SecurityErrorが消えたことを確認した。

対応後、self-testの`document.title`が **`C-OS local HTML QuickJS PASS`** に更新され、Browser title barへ実画面で反映された。これは外部script fetch、QuickJS実行、DocumentFragment、classList/style、localStorage、DOM Event dispatch、document.title bridgeを含むfixtureのtry節が例外なく完走した証跡である。画面PNGは `validation/secureboot_smp2_file_quickjs_storage_pass.png`。

本文が白紙になる件は別の問題として切り分けた。scriptを全く含まない `local_static_layout_selftest.html` では、NetSurfのtext plot座標が `(106,213)`, `(106,252)`, `(106,288)` と別々に出力され、Secure Boot SMP2の実画面でも3ブロック本文が正しく描画された（`validation/secureboot_smp2_static_layout.png`）。従って基礎NetSurf layout／plotterは動作しており、残る白紙化はQuickJS DOM mutation後のrebox経路に限定される。


## 初期DOM mutation reboxの修正と視覚的QuickJS PASS

空白本文の原因は基礎NetSurf layoutではなく、**parser実行中のscriptがDOMを変更した直後に、初回`dom_to_box`変換とは別の破壊的reboxを予約していたこと**だった。初回HTML conversionがまだ`CONTENT_STATUS_LOADING`である間のDOM変更については、live DOMをそのまま初回box構築に渡し、READY/DONE後のユーザー操作由来mutationだけをrebox対象とするよう`cos_netsurf_browser_notify_dom_mutation()`を修正した。

Secure Boot SMP2で修正を再検証した結果、外部script、localStorage、DocumentFragment、Event dispatchを含むself-testの本文text plotが別々の座標で発生し、Browser画面には `PASS: DocumentFragment: PASS | localStorage: PASS | Event dispatch: PASS | External script: PASS` と明確に表示された。title barも `C-OS local HTML QuickJS PASS` であり、実画面PNGは `validation/secureboot_smp2_file_quickjs_body_pass.png`。これでfile://実行の基盤と一般的な初期script mutationのレンダリング不具合を解消した。


## 添付 `index.html` の初期実行と入力経路

ユーザー提供の `index.html` をguest-visible FAT32の`/browser/index.html`から`file:///browser/index.html`としてSecure Boot SMP2へロードした。file fetchは2939 bytesを読み込み、QuickJSは1128 bytesのページscriptを例外なく実行し、NetSurfはtitle `QuickJS Compatible App` とフォーム・buttonを実描画した。初期画面は `validation/secureboot_smp2_attachment_index_initial.png`。

QMPのabsolute pointer/buttonイベントで描画上の「足し算を実行」buttonをクリックする試行は、画面更新を発生させなかった。これはC-OSの現在のQEMU USB HID入力配送／form focus経路がQMP injected pointerへまだ接続されていない、またはフォームinputが表示上見えないという入力統合の残課題である。QuickJS click listener自体はfile self-test上のsynthetic dispatchで確認済みだが、添付フォームをユーザー相当入力で実演するにはこの実HID→NetSurfフォーム経路の修正が必要である。


## 添付 `index.html` の実USB HID click→QuickJS→DOM更新

QMPのabsolute入力はstrict launcherのrelative `usb-mouse`構成では受理されなかった。relative移動でカーソルをbutton上に置いた後、**pressとreleaseを別QMP送信にして1秒間隔を置く**ことでUSB HID reportのedgeがGUI loopへ確実に届いた。`input-send-event`の同一batch内でpress/releaseを送る方法ではUSB polling側で状態が畳み込まれ、clickが失われ得ることも確認した。

分離操作後、添付`index.html`の表示は `エラー: 正しい数値を入力してください。` に変化した（`validation/secureboot_smp2_attachment_index_after_separated_click.png`）。これは、NetSurf hit-test、C-OS Browser click dispatch、real libdom event listener、QuickJS handler、`input.value`読出し、`isNaN`分岐、`outputDiv.textContent`更新、初期conversion中以外のreboxが連動して実行された実画面証跡である。次はinput focusと数値keyboard入力を使い、正の算術結果まで検証する。
